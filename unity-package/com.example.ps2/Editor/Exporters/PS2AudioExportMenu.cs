// M10 audio verification asset (plan section 9, M10 task 1).
//
//   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2AudioExportMenu.ExportAudioScene \
//         -ps2Output <audio.p2b> -ps2Music <music.raw>
//
// Clips are generated procedurally rather than imported, so the asset is
// deterministic and the repository carries no audio: a short blip, a longer
// tone, and a sweep whose length the on-target test uses to prove the SPU2
// really consumed the sample (a voice that never stops is a voice that never
// played). The streamed music track is written as raw 16-bit stereo PCM.
using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Ps2.Editor
{
    internal static class PS2AudioExportMenu
    {
        private const int SfxRate = 22050;   // the plan's SFX rate
        private const int MusicRate = 22050;

        public static void ExportAudioScene()
        {
            string output = "audio.p2b";
            string music = "music.raw";
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output") output = args[i + 1];
                if (args[i] == "-ps2Music") music = args[i + 1];
            }

            var clips = new List<P2bAudioExporter.EncodedClip>
            {
                // 0: a 0.25 s blip -- short enough that the on-target test can
                //    watch it finish inside a handful of frames.
                P2bAudioExporter.Encode(MakeTone("blip", 880.0f, 0.25f), false, SfxRate),
                // 1: a 1.5 s tone for voice-stealing and overlap tests.
                P2bAudioExporter.Encode(MakeTone("tone", 440.0f, 1.5f), false, SfxRate),
                // 2: a 2 s sweep, the most demanding thing for a 4-bit
                //    encoder because the waveform never settles.
                P2bAudioExporter.Encode(MakeSweep("sweep", 200.0f, 3000.0f, 2.0f),
                                        false, SfxRate),
            };

            foreach (P2bAudioExporter.EncodedClip clip in clips)
            {
                Debug.Log("[PS2] clip '" + clip.Name + "': " + clip.SampleCount +
                          " samples at " + clip.Frequency + " Hz, " +
                          clip.Bytes.Length + " bytes, SNR " +
                          clip.SnrDb.ToString("F1") + " dB");
            }

            // A scene with just a camera: the audio test needs the container,
            // not geometry.
            Scene scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                UnityEngine.Object.DestroyImmediate(root);
            }
            var camGo = new GameObject("Camera");
            Camera cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 60.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 100.0f;

            P2bSceneExporter.PendingSound = P2bAudioExporter.BuildSection(clips);
            P2bSceneExporter.ExportActiveScene(output);
            P2bSceneExporter.PendingSound = null;

            WriteMusic(music, 8.0f);
            Debug.Log("[PS2] audio scene exported to " + output);
        }

        // Raw 16-bit stereo PCM for the streaming path: a slow chord that is
        // obviously wrong if the feeder drops or repeats a chunk.
        private static void WriteMusic(string path, float seconds)
        {
            int frames = (int)(MusicRate * seconds);
            var bytes = new byte[frames * 4];
            for (int i = 0; i < frames; i++)
            {
                double t = (double)i / MusicRate;
                double left = 0.4 * Math.Sin(2.0 * Math.PI * 220.0 * t) +
                              0.2 * Math.Sin(2.0 * Math.PI * 330.0 * t);
                double right = 0.4 * Math.Sin(2.0 * Math.PI * 277.0 * t) +
                               0.2 * Math.Sin(2.0 * Math.PI * 440.0 * t);
                short l = (short)Mathf.Clamp((float)(left * 32767.0), -32768, 32767);
                short r = (short)Mathf.Clamp((float)(right * 32767.0), -32768, 32767);
                bytes[i * 4 + 0] = (byte)(l & 0xFF);
                bytes[i * 4 + 1] = (byte)((l >> 8) & 0xFF);
                bytes[i * 4 + 2] = (byte)(r & 0xFF);
                bytes[i * 4 + 3] = (byte)((r >> 8) & 0xFF);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
            File.WriteAllBytes(path, bytes);
            Debug.Log("[PS2] music track: " + seconds + " s stereo at " +
                      MusicRate + " Hz -> " + path + " (" + bytes.Length + " bytes)");
        }

        private static AudioClip MakeTone(string name, float frequency, float seconds)
        {
            int samples = (int)(SfxRate * seconds);
            var data = new float[samples];
            for (int i = 0; i < samples; i++)
            {
                float t = (float)i / SfxRate;
                // A short fade at both ends: a hard edge is a click, and a
                // click is indistinguishable from an encoder bug by ear.
                float envelope = Mathf.Min(1.0f, Mathf.Min(t * 40.0f,
                                                           (seconds - t) * 40.0f));
                data[i] = 0.6f * envelope * Mathf.Sin(2.0f * Mathf.PI * frequency * t);
            }
            AudioClip clip = AudioClip.Create(name, samples, 1, SfxRate, false);
            clip.SetData(data, 0);
            return clip;
        }

        private static AudioClip MakeSweep(string name, float from, float to,
                                           float seconds)
        {
            int samples = (int)(SfxRate * seconds);
            var data = new float[samples];
            double phase = 0.0;
            for (int i = 0; i < samples; i++)
            {
                float t = (float)i / SfxRate;
                double frequency = from + (to - from) * (t / seconds);
                phase += 2.0 * Math.PI * frequency / SfxRate;
                float envelope = Mathf.Min(1.0f, Mathf.Min(t * 40.0f,
                                                           (seconds - t) * 40.0f));
                data[i] = 0.6f * envelope * (float)Math.Sin(phase);
            }
            AudioClip clip = AudioClip.Create(name, samples, 1, SfxRate, false);
            clip.SetData(data, 0);
            return clip;
        }
    }
}
