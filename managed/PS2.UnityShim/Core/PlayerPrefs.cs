using System.Collections.Generic;
using System.Text;
using UnityEngine.Internal;

namespace UnityEngine
{
    // PlayerPrefs over the M10 memcard store (plan 7.1, M12.5 task 3).
    //
    // SUBSET DISCIPLINE (ADR-007): Get/Set/HasKey/Delete behave exactly as
    // Unity's, including immediate visibility of a Set before any Save. The
    // deviations are the hardware's, and they are documented rather than
    // papered over (docs/supported-api.md 28-29):
    //
    //   28. The store is finite: 64 keys, 24-byte keys, 48-byte string
    //       values. A Set beyond a limit logs an error naming it and does
    //       not store -- Unity's version is unbounded.
    //   29. Save() can FAIL: a memory card can be absent, unformatted, full
    //       or write-protected, states Unity's API has no words for. Save()
    //       stays void to match Unity; the result lands in
    //       PS2Memory.LastSaveError, and values survive a power cycle only
    //       after a Save that reported Ok. There is no auto-save on quit --
    //       a PS2 game has no quit.
    public static class PlayerPrefs
    {
        // String values read back byte-wise over the bridge (a returned
        // pointer would need an owner), so they are cached here. Coherent
        // because only this class mutates the native store.
        private static readonly Dictionary<string, string> s_StringCache =
            new Dictionary<string, string>();

        public static void SetInt(string key, int value)
        {
            if (CheckKey(key)) Native.ps2ur_prefs_set_int(key, value);
        }

        public static void SetFloat(string key, float value)
        {
            if (CheckKey(key)) Native.ps2ur_prefs_set_float(key, value);
        }

        public static void SetString(string key, string value)
        {
            if (!CheckKey(key)) return;
            value = value ?? "";
            if (Encoding.UTF8.GetByteCount(value) > 48)
            {
                Debug.LogError(
                    "PlayerPrefs.SetString: value for '" + key + "' is over " +
                    "the 48-byte card limit and was not stored. Deviation 28 " +
                    "in docs/supported-api.md.");
                return;
            }
            Native.ps2ur_prefs_set_string(key, value);
            s_StringCache[key] = value;
        }

        public static int GetInt(string key) => GetInt(key, 0);
        public static int GetInt(string key, int defaultValue) =>
            Native.ps2ur_prefs_get_int(key, defaultValue);

        public static float GetFloat(string key) => GetFloat(key, 0f);
        public static float GetFloat(string key, float defaultValue) =>
            Native.ps2ur_prefs_get_float(key, defaultValue);

        public static string GetString(string key) => GetString(key, "");

        public static string GetString(string key, string defaultValue)
        {
            if (s_StringCache.TryGetValue(key, out string cached))
                return cached;
            int length = Native.ps2ur_prefs_string_length(key);
            if (length < 0)
                return defaultValue;
            var bytes = new byte[length];
            for (int i = 0; i < length; i++)
                bytes[i] = (byte)Native.ps2ur_prefs_string_byte(key, i);
            string value = Encoding.UTF8.GetString(bytes);
            s_StringCache[key] = value;
            return value;
        }

        public static bool HasKey(string key) =>
            Native.ps2ur_prefs_has_key(key) != 0;

        public static void DeleteKey(string key)
        {
            Native.ps2ur_prefs_delete_key(key);
            s_StringCache.Remove(key);
        }

        public static void DeleteAll()
        {
            Native.ps2ur_prefs_delete_all();
            s_StringCache.Clear();
        }

        public static void Save()
        {
            PS2Memory.LastSaveError =
                (PS2SaveStatus)Native.ps2ur_prefs_save();
            if (PS2Memory.LastSaveError != PS2SaveStatus.Ok)
            {
                Debug.LogWarning("PlayerPrefs.Save failed: " +
                                 PS2Memory.LastSaveError +
                                 " (PS2Memory.LastSaveError; deviation 29).");
            }
        }

        private static bool CheckKey(string key)
        {
            if (string.IsNullOrEmpty(key))
                return false;
            if (Encoding.UTF8.GetByteCount(key) > 24)
            {
                Debug.LogError(
                    "PlayerPrefs: key '" + key + "' is over the 24-byte card " +
                    "limit and was not stored. Deviation 28 in " +
                    "docs/supported-api.md.");
                return false;
            }
            return true;
        }
    }

    // Mirrors memcard::Status; what a real card can actually say.
    public enum PS2SaveStatus
    {
        Ok = 0,
        NoCard,
        NotFormatted,
        Full,
        WriteProtected,
        NotFound,
        Error,
    }

    // The PS2-specific state Unity's API has no vocabulary for, in the same
    // spirit as PS2Input (M10).
    public static class PS2Memory
    {
        public static PS2SaveStatus LastSaveError { get; internal set; } =
            PS2SaveStatus.Ok;
    }
}
