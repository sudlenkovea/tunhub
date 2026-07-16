using System.Diagnostics;

namespace TunHub.Engine.Platform;

public readonly record struct CommandResult(int ExitCode, string Stdout, string Stderr)
{
    public bool Ok => ExitCode == 0;
}

/// <summary>Runs external processes (route/netsh/pfctl/…) and captures output.</summary>
public static class Shell
{
    public static CommandResult Run(string path, params string[] args) => Run(path, args, null);

    public static CommandResult Run(string path, IEnumerable<string> args, string? stdin)
    {
        var psi = new ProcessStartInfo
        {
            FileName = path,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = stdin is not null,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var a in args) psi.ArgumentList.Add(a);

        try
        {
            using var p = Process.Start(psi)!;
            if (stdin is not null)
            {
                p.StandardInput.Write(stdin);
                p.StandardInput.Close();
            }
            var so = p.StandardOutput.ReadToEnd();
            var se = p.StandardError.ReadToEnd();
            p.WaitForExit();
            return new CommandResult(p.ExitCode, so, se);
        }
        catch (Exception ex)
        {
            return new CommandResult(-1, "", ex.Message);
        }
    }
}
