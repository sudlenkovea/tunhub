# План: добавление OpenVPN (с OTP / challenge-response) в TunHub

## TL;DR

OpenVPN принципиально отличается от WireGuard/AmneziaWG:

| | WireGuard / AmneziaWG | OpenVPN |
|---|---|---|
| Конфиг | `wg-quick .conf` | `.ovpn` (совсем другой формат) |
| Ядро | `wireguard-go` / `amneziawg-go`, UAPI-сокет | бинарь `openvpn`, **management-интерфейс** |
| Аутентификация | статические ключи, всё известно заранее | user/pass + сертификаты + **OTP/challenge во время коннекта** |
| Маршруты/DNS | считаем сами из AllowedIPs | **пушит сервер** (узнаём при подключении) |
| Состояние | handshake | конечный автомат CONNECTING→WAIT→AUTH→GET_CONFIG→ASSIGN_IP→CONNECTED |

Архитектура TunHub (SwiftUI-app ↔ XPC ↔ root-демон, который спавнит «ядра» и говорит с ними по сокету, а также рулит route/DNS/pf) **подходит** — большинство слоёв переиспользуется. Главное новое: **обратный канал демон→приложение** для запроса логина/пароля/OTP в процессе подключения (сейчас модель односторонняя: приложение отдаёт готовый spec демону).

Рекомендация по ядру: **бандлить community-бинарь `openvpn` 2.6** и управлять им через management-интерфейс — это 1-в-1 ложится на модель «спавним ядро, общаемся по сокету». Альтернатива — `openvpn3` (`ovpncli`, C++ библиотека) — мощнее, но требует C++/ObjC++-моста в демоне.

---

## Что переиспользуется без изменений

- Root-демон: жизненный цикл процессов, XPC-сервис, `RouteManager`, `DNSManager`, `FirewallManager` (kill switch), crash-recovery, ownership-реестр.
- App-оболочка: menu bar, окна, `TunnelStore`, секреты в Keychain (комбинированный элемент), логи, локализация, автозапуск/автоконнект.
- Модель health по трафику (bytecount вместо handshake).

## Что придётся переписать / отрефакторить

1. **`TunnelKind`** (`Models.swift`): добавить `case openvpn`; `coreBinary` → `"openvpn"`.
2. **`TunnelConfig` / `ResolvedTunnelSpec`**: `.ovpn` не влезает в WG-модель. Добавить параллельную ветку `openvpn: OpenVPNProfile?` (как сейчас `awg`). Хранить сырой текст `.ovpn` (секреты вынесены в Keychain-ссылки) + разобранные метаданные.
3. **`ConflictChecker`**: сделать kind-aware. У OpenVPN маршруты приходят от сервера — до коннекта их нет, поэтому pre-start проверять только endpoint/порт и (если `redirect-gateway`) конфликт default-route; полную проверку роутов делать **после** коннекта из пуш-опций.
4. **XPC-протокол** (`XPCProtocol.swift`): добавить (а) поле `authRequest` в `TunnelRuntimeState` (id, тип: userpass / static-challenge / dynamic-challenge, текст-подсказка, echo-флаг) и (б) метод `provideCredentials(id, username, password, otp)`. Это и есть тот самый обратный канал.
5. **`TunnelSupervisor`**: вынести общий протокол-диспетчер, чтобы WG- и OpenVPN-супервайзеры сосуществовали; `start()` роутит по `kind`.
6. **`ImportService`**: детект `.ovpn` vs `.conf` (по расширению/содержимому), маршрутизация в нужный парсер. ZIP-импорт — уже общий.
7. **`build.sh`**: собрать/забандлить `openvpn` (deps: openssl, lzo, lz4) в `Contents/MacOS/openvpn`, подписать вместе с остальными Mach-O.
8. **`DNSManager` / `FirewallManager`**: минорно — принимать пуш-DNS от OpenVPN и endpoint (ip:port из `remote`) для kill switch.

## Что дописать (новые файлы)

### Shared
- **`OVPNParser.swift`** — парсер `.ovpn`: `remote` (несколько, failover), `proto`/`port`, инлайн-блоки `<ca> <cert> <key> <tls-auth> <tls-crypt> <tls-crypt-v2>`, `auth-user-pass` (с/без инлайн-кред), **`static-challenge "текст" echo`**, `cipher`/`data-ciphers`, `auth`, `comp-lzo`/`compress` (принимать, но предупреждать про VORACLE), `redirect-gateway`, `dhcp-option DNS/DOMAIN`, `route`/`route-nopull`, `remote-cert-tls`, `key-direction`, `pkcs12`. Скрипты (`up`/`down`/`--script-security`) **не исполняем** — вырезаем и предупреждаем (та же политика, что с PostUp).
- **`OpenVPNProfile`** (модель): remotes, proto/port, authMode (cert / user-pass / user-pass+cert), needsUsername, staticChallenge (text + echo), redirectGateway, cipher и т.д.

### Daemon
- **`OpenVPNSupervisor.swift`** — параллель `TunnelSupervisor`:
  - пишет `.ovpn` во временный файл (0600, root) с разрешёнными инлайн-секретами;
  - запускает `openvpn --config <tmp> --management <unixsock> unix --management-client --management-hold --management-query-passwords --auth-nocache --auth-retry interact` (+ `--dev utun`, при необходимости `--route-noexec`);
  - подключается к management-сокету и ведёт диалог (см. ниже);
  - DNS: OpenVPN на macOS **сам DNS не ставит** → парсим пуш `dhcp-option DNS` и применяем через `DNSManager`; маршруты по умолчанию ставит сам OpenVPN (либо `--route-noexec` и ставим через `RouteManager`);
  - kill switch: отдаём utun + `remote ip:port` в `FirewallManager`;
  - стоп: `signal SIGTERM` в management.
- **`OpenVPNManagement.swift`** — клиент management-протокола: команды `state on`, `bytecount 5`, `hold release`; парсинг асинхронных `>STATE:`, `>PASSWORD:`, `>BYTECOUNT:`, `>INFO:`, `>HOLD:`.

### App / UI
- **Импорт `.ovpn`** (drag&drop + ZIP).
- **Редактор OpenVPN** — вью с (в основном) read-only сырым конфигом + поля кред: username, «сохранить пароль», индикатор static-challenge. Большинство `.ovpn` используются как есть, правок минимум.
- **Диалог ввода кред/OTP при подключении** — sheet: логин/пароль (+ OTP), появляется:
  - для **static-challenge** — до старта (OTP известен заранее, кладём в spec);
  - для **dynamic-challenge** — **в процессе** коннекта (управляется полем `authRequest` из runtime-состояния, которое приложение опрашивает так же, как статусы).
- **Статус**: маппинг состояний OpenVPN (CONNECTING/WAIT/AUTH/GET_CONFIG/CONNECTED) на фазы; пуш-DNS/маршруты — в разворачивающемся блоке Routes.
- Секреты (username/password/passphrase ключа) — в Keychain (комбинированный элемент, как сейчас).

---

## Механика OTP / challenge-response (ключевое)

Через management-интерфейс, когда нужны креды, OpenVPN присылает `>PASSWORD:Need 'Auth' username/password`. Отвечаем командами `username "Auth" <user>` и `password "Auth" <secret>`.

**Static challenge** (в конфиге `static-challenge "Enter OTP" 1`): пароль и OTP кодируются в base64 и склеиваются:
```
password "Auth" "SCRV1:<base64(пароль)>:<base64(otp)>"
```
Пример: пароль `bar`, OTP `8675309` → `SCRV1:YmFy:ODY3NTMwOQ==`. OTP спрашиваем у пользователя ДО старта.

**Dynamic challenge**: сервер после первой попытки присылает вызов в формате `CRV1`:
```
>PASSWORD:Need 'Auth' CRV1:<flags>:<state>:<base64(prompt)>
```
где `flags` — `E` (echo: показывать ввод) и `R` (response required), `state` — непрозрачная строка, `prompt` (base64) — человекочитаемый текст («Enter your OTP»). Декодируем prompt, **спрашиваем OTP у пользователя** (тот самый обратный канал), затем отвечаем в зависимости от FORMAT-флага: либо `CRV1::<state>::<response>`, либо (concat-режим) просто `<password><response>`. Также обрабатываем `>PASSWORD:Verification Failed` и повтор.

**auth-token**: сервер часто пушит `auth-token` — одноразовый токен, которым OpenVPN аутентифицируется при реконнекте, **не переспрашивая OTP**. Обязательно поддержать (management сам его применяет, нам нужно не терять management-сессию/не убивать процесс при реконнекте), иначе каждый разрыв = новый ввод OTP.

«И прочее, что позволяет OpenVPN» — заодно покрываем: `tls-auth`/`tls-crypt`/`tls-crypt-v2`, несколько `remote` с failover, TCP/UDP, IPv6, cert+password, PKCS#12, `remote-cert-tls server`.

---

## Порядок работ (майлстоуны)

1. `OVPNParser` + `OpenVPNProfile` + модель/kind (+ тесты парсера на реальных `.ovpn`).
2. Бандл `openvpn` в build.sh.
3. `OpenVPNManagement` + `OpenVPNSupervisor` (без OTP): cert-only или user/pass из Keychain, поднять туннель, bytecount, DNS из пуша, kill switch.
4. XPC обратный канал (`authRequest` + `provideCredentials`) + static-challenge (OTP до старта).
5. Dynamic challenge (CRV1) + auth-token (реконнект без переспроса).
6. UI: импорт, редактор, sheet ввода кред/OTP, статусы, Routes из пуша.
7. ConflictChecker kind-aware (post-connect по пуш-роутам).

## Риски / нюансы

- **Обратный канал** демон↔приложение — главное архитектурное дополнение (сейчас модель односторонняя). Рекомендуемый способ — не reverse-XPC, а поле `authRequest` в runtime-состоянии + метод `provideCredentials` (ложится на текущий polling, меньше XPC-обвязки).
- **openvpn — GPL**: бандлить/распространять можно, но подпись hardened runtime + спавн стороннего бинаря из демона (entitlements, notarization).
- **macOS DNS**: OpenVPN сам DNS не выставляет → берём из пуш-опций и ставим через наш `DNSManager`.
- **UX OTP при реконнекте**: без поддержки `auth-token` каждый разрыв = повторный OTP; это критично для юзабилити.

## Источники

- OpenVPN 3 (ovpncli) — https://github.com/OpenVPN/openvpn3
- OpenVPN Connect for macOS — https://openvpn.net/connect-docs/connect-for-macos.html
- Management Interface — https://openvpn.net/community-docs/management-interface.html
- Dynamic challenge (CRV1) — https://dev.to/fadi_hamwi_53647a58cfb6c0/dynamic-challenge-in-openvpn-4n5a
- Static-challenge concat mode (openvpn3 #428) — https://github.com/OpenVPN/openvpn3/issues/428
