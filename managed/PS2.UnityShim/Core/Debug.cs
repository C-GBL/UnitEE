using System.Runtime.CompilerServices;
using UnityEngine.Internal;

namespace UnityEngine
{
    // Routes to the native runtime's log (EE console / host tty via
    // ps2ur_debug_log) on target, or to System.Console in the Editor
    // play-mode stand-in. The switch is the runtime bool
    // ShimConfig.UseConsoleLog, NOT a compile-time #if, so one assembly
    // serves both worlds (see Internal/Native.cs).
    //
    // This is the workhorse debugging tool (plan M7 task 6): every entry
    // carries compiler-stamped source file/line via [CallerFilePath]/
    // [CallerLineNumber]. The optional parameters are invisible at Unity-
    // compatible call sites and cost nothing at runtime.
    public static class Debug
    {
        public static void Log(object message,
            [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Write("", message, file, line);

        public static void LogWarning(object message,
            [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Write("[warning] ", message, file, line);

        public static void LogError(object message,
            [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Write("[error] ", message, file, line);

        public static void Assert(bool condition,
            [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        {
            if (!condition) Write("[assert] ", "Assertion failed", file, line);
        }

        public static void Assert(bool condition, object message,
            [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        {
            if (!condition) Write("[assert] ", message, file, line);
        }

        private static void Write(string prefix, object message, string file, int line)
        {
            string text = prefix + (message != null ? message.ToString() : "Null");
            if (file.Length != 0)
                text = text + "  (" + FileName(file) + ":" + line + ")";
            if (ShimConfig.UseConsoleLog)
            {
                System.Console.WriteLine(text);
            }
            else
            {
                Native.ps2ur_debug_log(text);
            }
        }

        // Just the file name: full workstation paths are noise on a console
        // log and can exceed the EE-side line budget.
        private static string FileName(string path)
        {
            int cut = path.LastIndexOfAny(new[] { '/', '\\' });
            return cut < 0 ? path : path.Substring(cut + 1);
        }
    }
}
