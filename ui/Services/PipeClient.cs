using System.IO;
using System.IO.Pipes;
using System.Text;
using RA2Hook.RuntimeUI.Models;

namespace RA2Hook.RuntimeUI.Services;

public sealed class PipeClient
{
    private const string PipeName = "ra2hook-runtime-v1";
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async Task<IReadOnlyList<string[]>> SendAsync(
        string command, int timeoutMs = 500, CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(timeoutMs);
            await using var pipe = new NamedPipeClientStream(
                ".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(timeout.Token);

            await using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, leaveOpen: true)
            {
                AutoFlush = true
            };
            using var reader = new StreamReader(pipe, Encoding.UTF8, true, 1024, leaveOpen: true);
            await writer.WriteLineAsync(command);

            var rows = new List<string[]>();
            while (await reader.ReadLineAsync(timeout.Token) is { } line)
            {
                if (line == "END") break;
                rows.Add(line.Split('\t').Select(Decode).ToArray());
            }
            return rows;
        }
        finally
        {
            _gate.Release();
        }
    }

    public static RuntimeStatus? ParseStatus(IReadOnlyList<string[]> rows)
    {
        var row = rows.FirstOrDefault(value => value.Length >= 15 && value[0] == "STATUS");
        if (row is null) return null;
        return new RuntimeStatus(
            Flag(row[1]), Flag(row[2]), Flag(row[3]), Flag(row[4]),
            Flag(row[5]), Flag(row[6]), Flag(row[7]), Number(row[8]),
            Number(row[9]), Number(row[10]), Number(row[11]), row[12], row[13], row[14]);
    }

    public static IReadOnlyList<RuntimeItem> ParseItems(IReadOnlyList<string[]> rows) =>
        rows.Where(row => row.Length >= 7 && row[0] == "ITEM")
            .Select(row => new RuntimeItem(row[1], row[2], row[3], row[4], row[5], row[6]))
            .ToArray();

    private static bool Flag(string value) => value == "1";
    private static int Number(string value) => int.TryParse(value, out var number) ? number : 0;

    private static string Decode(string value)
    {
        var result = new StringBuilder(value.Length);
        for (var index = 0; index < value.Length; index++)
        {
            if (value[index] == '%' && index + 2 < value.Length &&
                byte.TryParse(value.AsSpan(index + 1, 2),
                    System.Globalization.NumberStyles.HexNumber, null, out var decoded))
            {
                result.Append((char)decoded);
                index += 2;
            }
            else
            {
                result.Append(value[index]);
            }
        }
        return result.ToString();
    }
}
