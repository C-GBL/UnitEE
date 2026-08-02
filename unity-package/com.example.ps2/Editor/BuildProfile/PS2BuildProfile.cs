using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>Target video region (plan section 3.3).</summary>
    public enum PS2Region
    {
        NTSC,
        PAL
    }

    /// <summary>
    /// Baseline video modes per plan section 3.3: NTSC 512x448i (plan baseline)
    /// or 640x448i. Progressive modes are out of scope for v0.
    /// </summary>
    public enum PS2VideoMode
    {
        Interlaced512x448,
        Interlaced640x448
    }

    /// <summary>Colour buffer format. 16-bit doubles the texture budget (15.2).</summary>
    public enum PS2ColourFormat
    {
        Psmct32,
        Psmct16
    }

    /// <summary>Managed code stripping, mirroring Unity's own levels.</summary>
    public enum PS2StrippingLevel
    {
        Disabled,
        Low,
        Medium,
        High
    }

    /// <summary>Native optimisation level. Size is the default: the ELF budget is 9 MB (15.1).</summary>
    public enum PS2Optimisation
    {
        Debug,
        Size,
        Speed
    }

    /// <summary>Where Build and Run sends the result (plan 13.1 Deploy).</summary>
    public enum PS2DeployTarget
    {
        Pcsx2,
        Ps2Client,
        UsbFolder,
        None
    }

    /// <summary>
    /// PS2 build profile asset per ADR-005: Unity does not permit registering new
    /// BuildTarget values, so this ScriptableObject mirrors the shape and vocabulary
    /// of Unity 6 Build Profiles (scene list, scripting defines, per-profile
    /// settings) and is consumed by <see cref="PS2BuildPipeline"/> via the
    /// Window > PS2 > Build Profiles window.
    ///
    /// The settings groups follow plan section 13.1 exactly, and the group order
    /// here is the order the inspector draws them.
    /// </summary>
    [CreateAssetMenu(menuName = "Build Profiles/PlayStation 2", fileName = "PS2BuildProfile")]
    public sealed class PS2BuildProfile : ScriptableObject
    {
        // ---- Scenes -------------------------------------------------------

        [Tooltip("Scenes included in the build, in load order. The first scene boots.")]
        public List<SceneAsset> scenes = new List<SceneAsset>();

        // ---- Player -------------------------------------------------------

        [Tooltip("Product name. Shown in the ISO volume label when that is left blank.")]
        public string productName = "PS2 Game";

        [Tooltip("ISO volume label. Upper case, at most 32 characters.")]
        public string volumeLabel = "PS2GAME";

        [Tooltip("Boot ELF name in ISO 8.3 form, upper case. A real title uses its " +
                 "serial, e.g. SLUS_123.45 -- non-American BIOS refuses to boot a " +
                 "disc whose executable name does not match the serial.")]
        public string bootElfName = "SLUS_900.01";

        [Tooltip("Disc serial, CCCC-NNNNN. Must correspond to bootElfName or a " +
                 "PAL/NTSC-J console will refuse the disc.")]
        public string discSerial = "SLUS-90001";

        public PS2Region region = PS2Region.NTSC;

        [Tooltip("Version string written into SYSTEM.CNF.")]
        public string version = "1.00";

        // ---- Video --------------------------------------------------------

        [Tooltip("Plan section 3.3 baseline: 512x448 interlaced NTSC, PSMCT32 " +
                 "double-buffered colour + PSMZ24 Z.")]
        public PS2VideoMode videoMode = PS2VideoMode.Interlaced512x448;

        [Tooltip("PSMCT16 halves the framebuffer cost and roughly doubles the " +
                 "texture budget, at the price of visible banding on gradients.")]
        public PS2ColourFormat colourFormat = PS2ColourFormat.Psmct32;

        // ---- Memory (plan 15.1) -------------------------------------------

        [Tooltip("bdwgc managed heap, MB. Plan 15.1 budgets 4 MB.")]
        public int managedHeapMb = 4;

        [Tooltip("Resident asset pool (meshes, animation, collision), MB. Budget: 6 MB.")]
        public int assetPoolMb = 6;

        [Tooltip("Streaming and staging buffers, MB. Budget: 2 MB.")]
        public int streamingBufferMb = 2;

        [Tooltip("Texture VRAM budget in KB. Section 3.3 baseline framebuffer layout " +
                 "uses ~2.75 MB of the 4 MB GS VRAM, leaving ~1.2 MB (1228 KB) for " +
                 "resident textures.")]
        public int textureBudgetKb = 1228;

        // ---- Scripting ----------------------------------------------------

        [Tooltip("Scripting define symbols passed to the Roslyn recompilation pass.")]
        public string[] scriptingDefines = new string[] { "UNITY_PS2" };

        [Tooltip("Extra arguments appended to the il2cpp --convert-to-cpp invocation.")]
        public string[] additionalIl2cppArgs = new string[0];

        public PS2StrippingLevel strippingLevel = PS2StrippingLevel.Medium;

        [Tooltip("link.xml preserving reflectively-used types. Mandatory for any " +
                 "reflective code (supported-api deviation 15).")]
        public string linkXmlPath = "";

        // ---- Native -------------------------------------------------------

        [Tooltip("Size is the default: the ELF budget is 9 MB (plan 15.1) and the " +
                 "EE's 16 KB instruction cache punishes a large hot loop more than " +
                 "it rewards unrolling.")]
        public PS2Optimisation optimisation = PS2Optimisation.Size;

        [Tooltip("C++ exceptions. Managed try/catch NEEDS these (M6 verified EH " +
                 "works on the EE); turning them off shrinks the ELF but any throw " +
                 "then terminates.")]
        public bool nativeExceptions = true;

        [Tooltip("Run ps2-packer on the ELF. Shrinks the disc image and the load " +
                 "time; costs a decompression pass at boot.")]
        public bool usePs2Packer = false;

        // ---- Content ------------------------------------------------------

        [Tooltip("Default maximum texture edge. The GS samples at most 1024, and a " +
                 "512 budget is what fits 1.2 MB of VRAM.")]
        public int textureMaxSize = 256;

        [Tooltip("Audio sample rate for exported SFX. 22050 is the plan baseline.")]
        public int audioSampleRate = 22050;

        [Tooltip("Treat content warnings (oversized or non-power-of-two textures) " +
                 "as errors instead of auto-resizing.")]
        public bool strictContent = false;

        // ---- Deploy -------------------------------------------------------

        public PS2DeployTarget deployTarget = PS2DeployTarget.Pcsx2;

        [Tooltip("PCSX2 executable. Blank means look it up from the environment and " +
                 "the known install locations.")]
        public string pcsx2Path = "";

        [Tooltip("ps2client host IP for deploying to a real console over the network.")]
        public string ps2ClientHost = "192.168.1.10";

        [Tooltip("Folder to copy the build into for wLaunchELF, e.g. a mounted USB stick.")]
        public string usbFolder = "";

        [Tooltip("Run the build as soon as it finishes.")]
        public bool autoRun = false;

        // ---- Development --------------------------------------------------

        [Tooltip("Development build: assertions on, symbols kept, the debug overlay " +
                 "available.")]
        public bool developmentBuild = true;

        [Tooltip("On-target profiler overlay (M13).")]
        public bool profiler = false;

        [Tooltip("Read assets over PCSX2's host: filesystem instead of from the ISO. " +
                 "Much faster to iterate; NOT representative of disc timing, which is " +
                 "why a load-time measurement must never be taken this way.")]
        public bool hostFilesystem = true;

        // ---- Packaging ----------------------------------------------------

        [Tooltip("If enabled, the Package stage writes SYSTEM.CNF and runs mkps2iso " +
                 "to produce game.iso in addition to game.elf.")]
        public bool buildIso = true;

        [Tooltip("Order files on the ISO by a recorded first-access trace " +
                 "(tools/disc/layout_planner.py). Blank uses alphabetical order, " +
                 "which makes the drive seek.")]
        public string discTracePath = "";

        [Tooltip("Build output directory. Relative paths are resolved against the project root.")]
        public string outputDirectory = "Builds/PS2";

        // ---- Derived ------------------------------------------------------

        /// <summary>
        /// SYSTEM.CNF's BOOT2 line. Kept here rather than in the packaging step
        /// so the inspector can show the user exactly what the console will be
        /// asked to run.
        /// </summary>
        public string Boot2Line => $"BOOT2 = cdrom0:\\{bootElfName};1";

        public string VideoModeToken => region == PS2Region.PAL ? "PAL" : "NTSC";

        /// <summary>Framebuffer width/height for the selected mode.</summary>
        public int FramebufferWidth =>
            videoMode == PS2VideoMode.Interlaced640x448 ? 640 : 512;

        public int FramebufferHeight => 448;

        /// <summary>
        /// The bytes a double-buffered colour target plus Z costs in VRAM, which
        /// is what the texture budget has to fit alongside. PSMZ24 occupies a
        /// full 32 bits per pixel in VRAM despite storing 24.
        /// </summary>
        public int FramebufferBytes
        {
            get
            {
                int pixels = FramebufferWidth * FramebufferHeight;
                int colourBpp = colourFormat == PS2ColourFormat.Psmct16 ? 2 : 4;
                int zBpp = colourFormat == PS2ColourFormat.Psmct16 ? 2 : 4;
                return pixels * colourBpp * 2 + pixels * zBpp;
            }
        }
    }
}
