using System;
using System.Diagnostics;
using System.IO;
using System.Text;

internal static class Launcher
{
    private static int Main(string[] args)
    {
        Console.InputEncoding = new UTF8Encoding(false);
        Console.OutputEncoding = new UTF8Encoding(false);
        try
        {
            string directory = AppDomain.CurrentDomain.BaseDirectory;
            string node = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "nodejs", "node.exe");
            string script = Path.Combine(directory, "key-provider.cjs");
            bool health = args.Length == 1 && args[0] == "--health";
            string request = health ? null : Console.ReadLine();
            ProcessStartInfo info = new ProcessStartInfo(node);
            info.Arguments = "\"" + script + "\"" + (health ? " --health" : "");
            info.WorkingDirectory = directory;
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardInput = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.StandardOutputEncoding = new UTF8Encoding(false);
            info.StandardErrorEncoding = new UTF8Encoding(false);
            using (Process process = new Process())
            {
                process.StartInfo = info;
                process.OutputDataReceived += (sender, line) => { if (line.Data != null) Console.WriteLine(line.Data); };
                process.ErrorDataReceived += (sender, line) => { if (line.Data != null) Console.Error.WriteLine(line.Data); };
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                if (!health && request != null) process.StandardInput.WriteLine(request);
                process.StandardInput.Close();
                process.WaitForExit();
                return process.ExitCode;
            }
        }
        catch
        {
            Console.WriteLine("{\"type\":\"result\",\"success\":false,\"error\":\"Failed to launch the local key provider.\"}");
            return 1;
        }
    }
}
