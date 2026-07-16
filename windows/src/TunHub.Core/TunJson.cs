using System.Text.Json;
using System.Text.Json.Serialization;

namespace TunHub.Core;

/// <summary>Serializes an <see cref="IpAddressRange"/> as "addr/prefix" (preserving user input).</summary>
public sealed class IpAddressRangeJsonConverter : JsonConverter<IpAddressRange>
{
    public override IpAddressRange Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        var s = reader.GetString() ?? "";
        return IpAddressRange.Parse(s)
               ?? throw new JsonException($"invalid CIDR range: {s}");
    }

    public override void Write(Utf8JsonWriter writer, IpAddressRange value, JsonSerializerOptions options)
        => writer.WriteStringValue($"{value.AddressString}/{value.Prefix}");
}

public static class TunJson
{
    public static readonly JsonSerializerOptions Options = Build(indented: true);
    public static readonly JsonSerializerOptions Compact = Build(indented: false);

    private static JsonSerializerOptions Build(bool indented)
    {
        var o = new JsonSerializerOptions
        {
            WriteIndented = indented,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
        };
        o.Converters.Add(new IpAddressRangeJsonConverter());
        o.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return o;
    }

    public static string Encode<T>(T value) => JsonSerializer.Serialize(value, Options);
    public static T? Decode<T>(string json) => JsonSerializer.Deserialize<T>(json, Options);
}
