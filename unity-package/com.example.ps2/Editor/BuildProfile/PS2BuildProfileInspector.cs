using UnityEditor;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Custom inspector for <see cref="PS2BuildProfile"/>, laid out in the
    /// groups of plan section 13.1 and drawn to read like Unity's own Build
    /// Profiles inspector (plan 13.2: "so it reads as a native feature").
    ///
    /// The budget readouts are the point of writing this by hand rather than
    /// letting Unity draw the default inspector: a number like "textureBudgetKb
    /// = 1228" means nothing on its own, but "1228 KB of 4 MB VRAM, 1140 KB
    /// left after the framebuffer" tells the user whether they can afford
    /// another texture.
    /// </summary>
    [CustomEditor(typeof(PS2BuildProfile))]
    public sealed class PS2BuildProfileInspector : UnityEditor.Editor
    {
        private static readonly string[] GroupNames =
        {
            "Scenes", "Player", "Video", "Memory", "Scripting", "Native",
            "Content", "Deploy", "Development", "Packaging",
        };

        private bool[] _expanded;

        private void OnEnable()
        {
            if (_expanded == null || _expanded.Length != GroupNames.Length)
            {
                _expanded = new bool[GroupNames.Length];
                for (int i = 0; i < _expanded.Length; i++)
                    _expanded[i] = true;
            }
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            var profile = (PS2BuildProfile)target;

            if (Group(0))
            {
                Field("scenes");
                EditorGUILayout.HelpBox(
                    "The first scene in the list boots. Scenes are addressed by " +
                    "NAME on the disc -- there is no build-index list " +
                    "(supported-api deviation 13).", MessageType.None);
            }

            if (Group(1))
            {
                Field("productName");
                Field("volumeLabel");
                Field("bootElfName");
                Field("discSerial");
                Field("region");
                Field("version");
                EditorGUILayout.HelpBox(
                    "SYSTEM.CNF will contain:\n" + PS2StepPackage.BuildSystemCnf(profile).TrimEnd(),
                    MessageType.None);
            }

            if (Group(2))
            {
                Field("videoMode");
                Field("colourFormat");
                DrawVramBudget(profile);
            }

            if (Group(3))
            {
                Field("managedHeapMb");
                Field("assetPoolMb");
                Field("streamingBufferMb");
                Field("textureBudgetKb");
                DrawMainRamBudget(profile);
            }

            if (Group(4))
            {
                Field("scriptingDefines");
                Field("additionalIl2cppArgs");
                Field("strippingLevel");
                Field("linkXmlPath");
            }

            if (Group(5))
            {
                Field("optimisation");
                Field("nativeExceptions");
                Field("usePs2Packer");
            }

            if (Group(6))
            {
                Field("textureMaxSize");
                Field("audioSampleRate");
                Field("strictContent");
            }

            if (Group(7))
            {
                Field("deployTarget");
                switch (profile.deployTarget)
                {
                    case PS2DeployTarget.Pcsx2:
                        Field("pcsx2Path");
                        break;
                    case PS2DeployTarget.Ps2Client:
                        Field("ps2ClientHost");
                        break;
                    case PS2DeployTarget.UsbFolder:
                        Field("usbFolder");
                        break;
                }
                Field("autoRun");
            }

            if (Group(8))
            {
                Field("developmentBuild");
                Field("profiler");
                Field("hostFilesystem");
                if (profile.hostFilesystem)
                {
                    EditorGUILayout.HelpBox(
                        "Reads from the build folder over PCSX2's host: (Build And Run " +
                        "enables it in PCSX2). host: reads have NO seek cost, so a load " +
                        "time measured this way says nothing about the disc. Turn this " +
                        "off for a disc-only boot, which is what a console sees.",
                        MessageType.Warning);
                }
                Field("bootLadder");
                if (profile.bootLadder)
                {
                    EditorGUILayout.HelpBox(
                        "Boot ladder: the console paints a colour per start-up step " +
                        "before the GS comes up (about a minute to boot), then red twice " +
                        "at main() and orange when the GS returns. Read the sequence " +
                        "against the table in docs/hardware-bring-up.md. Turn Host " +
                        "Filesystem off so this is the console boot. Diagnostic only.",
                        MessageType.Warning);
                }
            }

            if (Group(9))
            {
                Field("buildIso");
                Field("discTracePath");
                Field("outputDirectory");
                if (string.IsNullOrEmpty(profile.discTracePath))
                {
                    EditorGUILayout.HelpBox(
                        "Without a first-access trace, files are laid out boot-first " +
                        "then alphabetically and the drive seeks. Record one by " +
                        "running the game and saving the console log, then point " +
                        "this at it (plan M10 task 4).", MessageType.None);
                }
            }

            serializedObject.ApplyModifiedProperties();
        }

        private bool Group(int index)
        {
            _expanded[index] = EditorGUILayout.BeginFoldoutHeaderGroup(
                _expanded[index], GroupNames[index]);
            EditorGUILayout.EndFoldoutHeaderGroup();
            return _expanded[index];
        }

        private void Field(string name)
        {
            SerializedProperty property = serializedObject.FindProperty(name);
            if (property != null)
                EditorGUILayout.PropertyField(property, true);
        }

        private static void DrawVramBudget(PS2BuildProfile profile)
        {
            const int kVramKb = 4096;
            const int kFontKb = 128;
            int frameKb = profile.FramebufferBytes / 1024;
            int used = frameKb + profile.textureBudgetKb + kFontKb;
            int free = kVramKb - used;

            string text =
                $"VRAM: {frameKb} KB framebuffer + {profile.textureBudgetKb} KB textures " +
                $"+ {kFontKb} KB font = {used} KB of 4096 KB ({free} KB free)";
            EditorGUILayout.HelpBox(
                text, free < 0 ? MessageType.Error : MessageType.Info);
            if (free < 0 && profile.colourFormat == PS2ColourFormat.Psmct32)
            {
                EditorGUILayout.HelpBox(
                    "Switching the colour format to PSMCT16 halves the framebuffer " +
                    "cost and would free " + (frameKb / 2) + " KB, at the price of " +
                    "banding on gradients.", MessageType.Info);
            }
        }

        private static void DrawMainRamBudget(PS2BuildProfile profile)
        {
            // Plan 15.1's fixed costs, so the number shown is the whole
            // picture rather than just the three sliders above it.
            const int kElfAndRuntimeMb = 13;
            const int kStackAndSlackMb = 3;
            const int kReserveMb = 2;
            int configured = profile.managedHeapMb + profile.assetPoolMb +
                             profile.streamingBufferMb;
            int total = configured + kElfAndRuntimeMb + kStackAndSlackMb + kReserveMb;

            EditorGUILayout.HelpBox(
                $"Main RAM: {configured} MB configured + {kElfAndRuntimeMb} MB ELF and " +
                $"runtime + {kStackAndSlackMb} MB stack and slack + {kReserveMb} MB " +
                $"reserve = {total} MB of 32 MB",
                total > 32 ? MessageType.Error : MessageType.Info);
        }
    }
}
