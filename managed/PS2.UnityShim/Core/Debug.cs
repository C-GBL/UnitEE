using UnityEngine.Internal;

namespace UnityEngine
{
    // Routes to the native runtime's log (EE console / host tty via
    // ps2ur_debug_log) on target, or to System.Console in the Editor
    // play-mode stand-in. The switch is the runtime bool
    // ShimConfig.UseConsoleLog, NOT a compile-time #if, so one assembly
    // serves both worlds (see Internal/Native.cs).
    public static class Debug
    {
        public static void Log(object message) => Write("", message);
        public static void LogWarning(object message) => Write("[warning] ", message);
        public static void LogError(object message) => Write("[error] ", message);

        public static void Assert(bool condition)
        {
            if (!condition) Write("[assert] ", "Assertion failed");
        }

        public static void Assert(bool condition, object message)
        {
            if (!condition) Write("[assert] ", message);
        }

        private static void Write(string prefix, object message)
        {
            string text = prefix + (message != null ? message.ToString() : "Null");
            if (ShimConfig.UseConsoleLog)
            {
                System.Console.WriteLine(text);
            }
            else
            {
                Native.ps2ur_debug_log(text);
            }
        }
    }
}
