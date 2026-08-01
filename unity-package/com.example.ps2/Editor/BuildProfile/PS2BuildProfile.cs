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

    /// <summary>
    /// PS2 build profile asset per ADR-005: Unity does not permit registering new
    /// BuildTarget values, so this ScriptableObject mirrors the shape and vocabulary
    /// of Unity 6 Build Profiles (scene list, scripting defines, per-profile
    /// settings) and is consumed by <see cref="PS2BuildPipeline"/> via the
    /// Window > PS2 > Build Profiles window.
    /// </summary>
    [CreateAssetMenu(menuName = "Build Profiles/PlayStation 2", fileName = "PS2BuildProfile")]
    public sealed class PS2BuildProfile : ScriptableObject
    {
        [Tooltip("Scenes included in the build, in load order. The first scene boots.")]
        public List<SceneAsset> scenes = new List<SceneAsset>();

        [Tooltip("Scripting define symbols passed to the Roslyn recompilation pass.")]
        public string[] scriptingDefines = new string[] { "UNITY_PS2" };

        [Tooltip("Build output directory. Relative paths are resolved against the project root.")]
        public string outputDirectory = "Builds/PS2";

        public PS2Region region = PS2Region.NTSC;

        [Tooltip("Plan section 3.3 baseline: 512x448 interlaced NTSC, PSMCT32 double-buffered colour + PSMZ24 Z.")]
        public PS2VideoMode videoMode = PS2VideoMode.Interlaced512x448;

        [Tooltip("Texture VRAM budget in KB. Section 3.3 baseline framebuffer layout uses ~2.75 MB of the 4 MB GS VRAM, leaving ~1.2 MB (1228 KB) for resident textures.")]
        public int textureBudgetKb = 1228;

        [Tooltip("If enabled, the Package stage runs ps2-packer and mkps2iso to produce game.iso in addition to game.elf.")]
        public bool buildIso = true;
    }
}
