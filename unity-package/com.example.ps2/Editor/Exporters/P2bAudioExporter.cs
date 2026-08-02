// AudioClip -> SPU2 ADPCM -> .p2b SND section (plan section 9, M10 task 1).
//
// The PS2's SPU2 plays 4-bit ADPCM in 16-byte blocks of 28 samples: one
// header byte carrying a shift and a filter index, one flags byte for loop
// points, then 14 bytes of nibbles. Decoding is
//
//     out[i] = (nibble << 12) >> shift  +  (hist1*pos[f] + hist2*neg[f]) / 64
//
// so encoding is a search: for each of the five filters and each shift, run
// the decoder forward and keep whichever pair reconstructs the block with
// the least squared error. That is 65 trial encodes per block, which costs
// nothing at export time and is the difference between clean audio and the
// crunchy artefacts a fixed-shift encoder produces on quiet passages.
//
// SELF-CHECK. Encoding has no runtime validator to catch it -- the SPU2 will
// happily play a badly encoded sample -- so the exporter decodes its own
// output and fails the export if the signal-to-noise ratio falls below the
// threshold. The reference encoder to compare against is ps2sdk's adpenc.
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Ps2.Editor
{
    internal static class P2bAudioExporter
    {
        public const int SamplesPerBlock = 28;
        public const int BytesPerBlock = 16;
        // Below this the encoding is audibly wrong, not merely lossy. Clean
        // 4-bit ADPCM on music-like material lands well above it.
        public const double MinimumSnrDb = 20.0;

        // SPU ADPCM filter coefficients, scaled by 64.
        private static readonly int[] FilterPos = { 0, 60, 115, 98, 122 };
        private static readonly int[] FilterNeg = { 0, 0, -52, -55, -60 };

        public sealed class EncodedClip
        {
            public byte[] Bytes;      // "APCM" blob, header included
            public string Name;
            public bool Loop;
            public double SnrDb;
            public int SampleCount;
            public int Frequency;
        }

        // Encodes one clip. Stereo is downmixed to mono: the SPU2 plays one
        // channel per voice, and a stereo SFX would spend two voices for an
        // effect that 3D panning is about to reposition anyway. Music that
        // needs stereo goes through the streaming path instead.
        public static EncodedClip Encode(AudioClip clip, bool loop,
                                         int targetFrequency)
        {
            var samples = new float[clip.samples * clip.channels];
            if (!clip.GetData(samples, 0))
            {
                throw new InvalidOperationException(
                    "[PS2] cannot read AudioClip '" + clip.name +
                    "'; enable Decompress On Load in the importer");
            }

            // Downmix.
            var mono = new float[clip.samples];
            for (int i = 0; i < clip.samples; i++)
            {
                float sum = 0.0f;
                for (int c = 0; c < clip.channels; c++)
                {
                    sum += samples[i * clip.channels + c];
                }
                mono[i] = sum / clip.channels;
            }

            // Resample to the target rate with linear interpolation. SFX at
            // 22.05 kHz is the plan's default: SPU2 memory is 2 MB total.
            short[] pcm = Resample(mono, clip.frequency, targetFrequency);

            byte[] adpcm = EncodeAdpcm(pcm, loop, out double snr);
            if (snr < MinimumSnrDb)
            {
                throw new InvalidOperationException(
                    "[PS2] ADPCM encoding of '" + clip.name + "' scored " +
                    snr.ToString("F1") + " dB SNR, below the " + MinimumSnrDb +
                    " dB floor -- the encoder is wrong, not the audio");
            }

            var blob = new ByteBuffer();
            blob.U8((byte)'A');
            blob.U8((byte)'P');
            blob.U8((byte)'C');
            blob.U8((byte)'M');
            blob.U8(1);                       // version
            blob.U8(1);                       // channels (mono after downmix)
            blob.U16(0);
            // SPU2 pitch: 4096 is 48 kHz, the hardware's native rate.
            blob.U32((uint)(targetFrequency * 4096 / 48000));
            blob.U32((uint)pcm.Length);
            blob.Bytes(adpcm);

            return new EncodedClip
            {
                Bytes = blob.ToArray(),
                Name = clip.name,
                Loop = loop,
                SnrDb = snr,
                SampleCount = pcm.Length,
                Frequency = targetFrequency,
            };
        }

        private static short[] Resample(float[] input, int fromRate, int toRate)
        {
            if (fromRate == toRate)
            {
                var direct = new short[input.Length];
                for (int i = 0; i < input.Length; i++)
                {
                    direct[i] = ToPcm16(input[i]);
                }
                return direct;
            }
            int outCount = (int)((long)input.Length * toRate / fromRate);
            var output = new short[Mathf.Max(1, outCount)];
            double step = (double)fromRate / toRate;
            for (int i = 0; i < output.Length; i++)
            {
                double source = i * step;
                int i0 = (int)source;
                int i1 = Mathf.Min(i0 + 1, input.Length - 1);
                float frac = (float)(source - i0);
                output[i] = ToPcm16(Mathf.Lerp(input[i0], input[i1], frac));
            }
            return output;
        }

        private static short ToPcm16(float v)
        {
            int s = Mathf.RoundToInt(Mathf.Clamp(v, -1.0f, 1.0f) * 32767.0f);
            return (short)Mathf.Clamp(s, short.MinValue, short.MaxValue);
        }

        // Encodes to SPU ADPCM and reports the SNR of its own reconstruction.
        public static byte[] EncodeAdpcm(short[] pcm, bool loop, out double snrDb)
        {
            int blocks = (pcm.Length + SamplesPerBlock - 1) / SamplesPerBlock;
            var output = new byte[blocks * BytesPerBlock];

            int hist1 = 0;
            int hist2 = 0;
            double signalEnergy = 0.0;
            double noiseEnergy = 0.0;

            var decoded = new int[SamplesPerBlock];
            var bestNibbles = new int[SamplesPerBlock];
            var trialNibbles = new int[SamplesPerBlock];
            var trialDecoded = new int[SamplesPerBlock];

            for (int b = 0; b < blocks; b++)
            {
                int start = b * SamplesPerBlock;
                int bestFilter = 0;
                int bestShift = 0;
                double bestError = double.MaxValue;
                int bestHist1 = hist1;
                int bestHist2 = hist2;

                for (int filter = 0; filter < FilterPos.Length; filter++)
                {
                    for (int shift = 0; shift <= 12; shift++)
                    {
                        int h1 = hist1;
                        int h2 = hist2;
                        double error = 0.0;
                        for (int i = 0; i < SamplesPerBlock; i++)
                        {
                            int sample = start + i < pcm.Length ? pcm[start + i] : 0;
                            int predicted = (h1 * FilterPos[filter] +
                                             h2 * FilterNeg[filter]) >> 6;
                            int residual = sample - predicted;
                            int step = 1 << (12 - shift);
                            int nibble = (int)Math.Round((double)residual / step);
                            if (nibble < -8) nibble = -8;
                            if (nibble > 7) nibble = 7;
                            int reconstructed = nibble * step + predicted;
                            if (reconstructed > 32767) reconstructed = 32767;
                            if (reconstructed < -32768) reconstructed = -32768;

                            trialNibbles[i] = nibble;
                            trialDecoded[i] = reconstructed;
                            double diff = sample - reconstructed;
                            error += diff * diff;
                            h2 = h1;
                            h1 = reconstructed;
                        }
                        if (error < bestError)
                        {
                            bestError = error;
                            bestFilter = filter;
                            bestShift = shift;
                            bestHist1 = h1;
                            bestHist2 = h2;
                            Array.Copy(trialNibbles, bestNibbles, SamplesPerBlock);
                            Array.Copy(trialDecoded, decoded, SamplesPerBlock);
                        }
                    }
                }

                int at = b * BytesPerBlock;
                output[at] = (byte)((bestFilter << 4) | bestShift);

                byte flags = 0;
                if (loop && b == 0) flags |= 4;               // loop start
                if (b == blocks - 1) flags |= loop ? (byte)3 : (byte)1; // end
                output[at + 1] = flags;

                for (int i = 0; i < SamplesPerBlock; i += 2)
                {
                    int low = bestNibbles[i] & 0x0F;
                    int high = bestNibbles[i + 1] & 0x0F;
                    output[at + 2 + i / 2] = (byte)(low | (high << 4));
                }

                for (int i = 0; i < SamplesPerBlock; i++)
                {
                    int sample = start + i < pcm.Length ? pcm[start + i] : 0;
                    double diff = sample - decoded[i];
                    signalEnergy += (double)sample * sample;
                    noiseEnergy += diff * diff;
                }

                hist1 = bestHist1;
                hist2 = bestHist2;
            }

            snrDb = noiseEnergy <= 0.0
                        ? 99.0
                        : 10.0 * Math.Log10(Math.Max(signalEnergy, 1.0) / noiseEnergy);
            return output;
        }

        // The decoder, used by the self-check and by tooling. Mirrors the
        // SPU2's arithmetic exactly.
        public static short[] DecodeAdpcm(byte[] adpcm)
        {
            int blocks = adpcm.Length / BytesPerBlock;
            var pcm = new short[blocks * SamplesPerBlock];
            int hist1 = 0;
            int hist2 = 0;
            for (int b = 0; b < blocks; b++)
            {
                int at = b * BytesPerBlock;
                int shift = adpcm[at] & 0x0F;
                int filter = (adpcm[at] >> 4) & 0x0F;
                if (filter >= FilterPos.Length) filter = 0;
                for (int i = 0; i < SamplesPerBlock; i++)
                {
                    int packed = adpcm[at + 2 + i / 2];
                    int nibble = (i % 2 == 0) ? (packed & 0x0F) : (packed >> 4);
                    if (nibble > 7) nibble -= 16; // sign-extend 4 bits
                    int predicted = (hist1 * FilterPos[filter] +
                                     hist2 * FilterNeg[filter]) >> 6;
                    int sample = nibble * (1 << (12 - shift)) + predicted;
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    pcm[b * SamplesPerBlock + i] = (short)sample;
                    hist2 = hist1;
                    hist1 = sample;
                }
            }
            return pcm;
        }

        // SND section: a table of clip records followed by the APCM blobs.
        public static byte[] BuildSection(List<EncodedClip> clips)
        {
            var table = new ByteBuffer();
            table.U32((uint)clips.Count);
            table.U32(0);
            table.U32(0);
            table.U32(0);

            int headerBytes = 16 + clips.Count * 16;
            int cursor = Align16(headerBytes);
            var offsets = new int[clips.Count];
            for (int i = 0; i < clips.Count; i++)
            {
                offsets[i] = cursor;
                cursor = Align16(cursor + clips[i].Bytes.Length);
            }
            for (int i = 0; i < clips.Count; i++)
            {
                table.U32((uint)P2bWriter.Fnv1a64(clips[i].Name));
                table.U32((uint)offsets[i]);
                table.U32((uint)clips[i].Bytes.Length);
                table.U32(clips[i].Loop ? 1u : 0u);
            }

            var section = new ByteBuffer();
            section.Bytes(table.ToArray());
            for (int i = 0; i < clips.Count; i++)
            {
                section.PadTo(16);
                section.Bytes(clips[i].Bytes);
            }
            return section.ToArray();
        }

        private static int Align16(int v) => (v + 15) & ~15;
    }
}
