using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Validates the profile's own settings before any work happens (plan
    /// section 13.5).
    ///
    /// Every message here names the setting, states the constraint, and says
    /// what to do about it. The plan is explicit that this is the difference
    /// between a tool people use and a tool people abandon, so the message text
    /// gets the same care as the code: "Invalid boot ELF name" is useless,
    /// "'game.elf' is not ISO 8.3 -- use SLUS_900.01, and match discSerial to
    /// it or a PAL console will refuse the disc" is not.
    /// </summary>
    public static class PS2ProfileValidator
    {
        // ISO 9660 level 1, which is what a PS2 BIOS reads: up to 8
        // upper-case characters, a dot, up to 3 more.
        private static readonly Regex EightDotThree =
            new Regex(@"^[A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3}$", RegexOptions.Compiled);
        private static readonly Regex Serial =
            new Regex(@"^[A-Z]{4}-[0-9]{5}$", RegexOptions.Compiled);

        public static void Validate(PS2BuildContext ctx)
        {
            PS2BuildProfile p = ctx.Profile;
            var errors = new List<string>();

            if (string.IsNullOrWhiteSpace(p.bootElfName) ||
                !EightDotThree.IsMatch(p.bootElfName))
            {
                errors.Add(
                    $"Boot ELF name '{p.bootElfName}' is not a valid ISO 8.3 name. " +
                    "It must be up to 8 upper-case characters, a dot, then up to 3 " +
                    "(for example SLUS_900.01). The BIOS reads this out of " +
                    "SYSTEM.CNF and will not find a file it cannot name.");
            }

            if (string.IsNullOrWhiteSpace(p.discSerial) || !Serial.IsMatch(p.discSerial))
            {
                errors.Add(
                    $"Disc serial '{p.discSerial}' is not in CCCC-NNNNN form " +
                    "(for example SLUS-90001).");
            }
            else if (EightDotThree.IsMatch(p.bootElfName ?? ""))
            {
                // The serial and the executable name have to correspond, or a
                // non-American BIOS refuses to boot. mkps2iso's own docs call
                // this out and it is the single most common way a
                // freshly-burned disc fails to start.
                string fromSerial = p.discSerial.Replace("-", "");
                string fromElf = p.bootElfName.Replace("_", "").Replace(".", "");
                if (!string.Equals(fromSerial, fromElf, StringComparison.OrdinalIgnoreCase))
                {
                    errors.Add(
                        $"Disc serial '{p.discSerial}' does not correspond to boot ELF " +
                        $"'{p.bootElfName}'. A PAL or NTSC-J console refuses to boot a " +
                        "disc whose executable name does not match the serial: " +
                        $"serial SLUS-90001 needs an executable named SLUS_900.01.");
                }
            }

            if (string.IsNullOrWhiteSpace(p.volumeLabel) || p.volumeLabel.Length > 32)
            {
                errors.Add(
                    $"ISO volume label '{p.volumeLabel}' must be 1 to 32 characters.");
            }

            // Memory budgets, against plan 15.1's 32 MB with a 2 MB reserve
            // that is explicitly not to be spent.
            int budget = p.managedHeapMb + p.assetPoolMb + p.streamingBufferMb;
            const int kElfAndRuntimeMb = 13; // 9 text + 2 data + 2 CLR overhead
            const int kStackAndSlackMb = 3;
            const int kReserveMb = 2;
            int total = budget + kElfAndRuntimeMb + kStackAndSlackMb + kReserveMb;
            if (total > 32)
            {
                errors.Add(
                    $"The memory budget does not fit in 32 MB: managed heap " +
                    $"{p.managedHeapMb} + asset pool {p.assetPoolMb} + streaming " +
                    $"{p.streamingBufferMb} = {budget} MB, plus ~{kElfAndRuntimeMb} MB " +
                    $"of ELF and runtime, ~{kStackAndSlackMb} MB of stack and slack, " +
                    $"and the {kReserveMb} MB reserve plan 15.1 says not to spend, " +
                    $"comes to {total} MB. Reduce one of the three.");
            }

            // VRAM: the framebuffer plus the texture budget must fit in 4 MB.
            const int kVramBytes = 4 * 1024 * 1024;
            const int kFontPinnedBytes = 128 * 1024;
            int vram = p.FramebufferBytes + p.textureBudgetKb * 1024 + kFontPinnedBytes;
            if (vram > kVramBytes)
            {
                errors.Add(
                    $"The VRAM budget does not fit in 4 MB: a {p.FramebufferWidth}x" +
                    $"{p.FramebufferHeight} {(p.colourFormat == PS2ColourFormat.Psmct16 ? "16" : "32")}-bit " +
                    $"double-buffered target plus Z is {p.FramebufferBytes / 1024} KB, " +
                    $"the texture budget is {p.textureBudgetKb} KB, and the pinned font " +
                    $"page is {kFontPinnedBytes / 1024} KB -- {vram / 1024} KB in total. " +
                    "Lower the texture budget, or switch the colour format to PSMCT16 " +
                    "which roughly doubles what fits.");
            }

            if (p.textureMaxSize < 8 || p.textureMaxSize > 1024)
            {
                errors.Add(
                    $"Texture max size {p.textureMaxSize} is out of range. The GS " +
                    "samples at most 1024 on an edge, and below 8 is not a useful " +
                    "texture.");
            }
            else if ((p.textureMaxSize & (p.textureMaxSize - 1)) != 0)
            {
                errors.Add(
                    $"Texture max size {p.textureMaxSize} is not a power of two. The " +
                    "GS addresses textures by log2 dimensions; a non-power-of-two " +
                    "size cannot be expressed in TEX0.");
            }

            if (p.audioSampleRate < 8000 || p.audioSampleRate > 48000)
            {
                errors.Add(
                    $"Audio sample rate {p.audioSampleRate} Hz is outside the range " +
                    "the SPU2 handles usefully (8000 to 48000).");
            }

            if (!string.IsNullOrEmpty(p.linkXmlPath) && !File.Exists(p.linkXmlPath))
            {
                errors.Add(
                    $"link.xml was set to '{p.linkXmlPath}' but no file is there. " +
                    "Either point it at a real file or clear the setting.");
            }

            if (p.deployTarget == PS2DeployTarget.UsbFolder &&
                string.IsNullOrWhiteSpace(p.usbFolder))
            {
                errors.Add("Deploy target is USB folder but no folder is set.");
            }

            // Warnings: things that will work but probably are not what was
            // meant.
            if (!p.nativeExceptions)
            {
                ctx.Warn(
                    "Native exceptions are off. Managed try/catch needs them (M6 " +
                    "verified C++ EH works on the EE); with them off, any throw " +
                    "terminates instead of being caught.");
            }
            if (p.hostFilesystem)
            {
                ctx.Warn(
                    "The host: filesystem is enabled. Iteration is much faster, but " +
                    "host: reads have NO seek cost, so any load-time measurement " +
                    "taken this way is meaningless -- measure from the ISO.");
            }
            if (p.developmentBuild)
            {
                ctx.Warn("This is a development build: assertions and symbols are " +
                         "included, and the ELF is larger than a shipping one.");
            }

            if (errors.Count > 0)
            {
                throw new PS2BuildException(
                    "The build profile has " + errors.Count +
                    (errors.Count == 1 ? " problem:\n  - " : " problems:\n  - ") +
                    string.Join("\n  - ", errors));
            }
        }
    }
}
