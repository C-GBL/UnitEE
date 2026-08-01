using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEditorInternal;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Dedicated build window per ADR-005, styled after the Unity 6 Build Profiles
    /// vocabulary: active profile field, reorderable scene list, per-profile
    /// settings foldout, and Build / Build And Run / Clean buttons wired to
    /// <see cref="PS2BuildPipeline"/>. The active profile is remembered per
    /// project via EditorPrefs.
    /// </summary>
    public sealed class PS2BuildProfileWindow : EditorWindow
    {
        private const string ProfilePrefKeyBase = "Ps2.Editor.ActiveProfileGuid";

        private PS2BuildProfile _profile;
        private SerializedObject _serializedProfile;
        private ReorderableList _sceneList;
        private bool _settingsFoldout = true;
        private Vector2 _scroll;

        [MenuItem("Window/PS2/Build Profiles")]
        public static void Open()
        {
            PS2BuildProfileWindow window = GetWindow<PS2BuildProfileWindow>();
            window.titleContent = new GUIContent("PS2 Build Profiles");
            window.minSize = new Vector2(380f, 420f);
            window.Show();
        }

        private void OnEnable()
        {
            LoadRememberedProfile();
            RebuildSerializedState();
        }

        private void OnGUI()
        {
            _scroll = EditorGUILayout.BeginScrollView(_scroll);

            DrawProfileField();

            if (_profile == null)
            {
                EditorGUILayout.HelpBox(
                    "Assign a PS2 build profile, or create one via Assets > Create > Build Profiles > PlayStation 2.",
                    MessageType.Info);
                EditorGUILayout.EndScrollView();
                return;
            }

            if (_serializedProfile == null || _sceneList == null)
            {
                RebuildSerializedState();
            }

            _serializedProfile.Update();
            EditorGUILayout.Space();
            _sceneList.DoLayoutList();
            DrawSettings();
            _serializedProfile.ApplyModifiedProperties();

            List<PS2ValidationMessage> messages = PS2ProjectValidator.Validate(_profile);
            DrawValidation(messages);
            DrawButtons(messages);

            EditorGUILayout.EndScrollView();
        }

        // ------------------------------------------------------------------ drawing

        private void DrawProfileField()
        {
            EditorGUI.BeginChangeCheck();
            PS2BuildProfile next = (PS2BuildProfile)EditorGUILayout.ObjectField(
                "Active Profile", _profile, typeof(PS2BuildProfile), false);
            if (EditorGUI.EndChangeCheck())
            {
                _profile = next;
                RememberProfile();
                RebuildSerializedState();
            }
        }

        private void DrawSettings()
        {
            _settingsFoldout = EditorGUILayout.Foldout(_settingsFoldout, "Platform Settings", true);
            if (!_settingsFoldout)
            {
                return;
            }
            EditorGUI.indentLevel++;
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("region"));
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("videoMode"));
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("textureBudgetKb"));
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("buildIso"));
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("outputDirectory"));
            EditorGUILayout.PropertyField(_serializedProfile.FindProperty("scriptingDefines"), true);
            EditorGUI.indentLevel--;
        }

        private void DrawValidation(List<PS2ValidationMessage> messages)
        {
            for (int i = 0; i < messages.Count; i++)
            {
                PS2ValidationMessage m = messages[i];
                EditorGUILayout.HelpBox(m.message + "\n" + m.fixHint, ToMessageType(m.severity));
            }
        }

        private void DrawButtons(List<PS2ValidationMessage> messages)
        {
            bool hasErrors = PS2ProjectValidator.HasErrors(messages);
            if (hasErrors)
            {
                EditorGUILayout.HelpBox("Build is disabled until the validation errors above are fixed.", MessageType.Error);
            }

            EditorGUILayout.Space();
            using (new EditorGUI.DisabledScope(hasErrors))
            {
                EditorGUILayout.BeginHorizontal();
                if (GUILayout.Button("Build", GUILayout.Height(28f)))
                {
                    RunPipeline(() => PS2BuildPipeline.Build(_profile));
                }
                if (GUILayout.Button("Build And Run", GUILayout.Height(28f)))
                {
                    RunPipeline(() => PS2BuildPipeline.BuildAndRun(_profile));
                }
                EditorGUILayout.EndHorizontal();
            }
            if (GUILayout.Button("Clean"))
            {
                RunPipeline(() => PS2BuildPipeline.Clean(_profile));
            }
        }

        private static void RunPipeline(Action action)
        {
            try
            {
                action();
            }
            catch (PS2BuildException e)
            {
                Debug.LogError("[PS2] build failed: " + e.Message);
            }
        }

        private static MessageType ToMessageType(PS2ValidationSeverity severity)
        {
            switch (severity)
            {
                case PS2ValidationSeverity.Error:
                    return MessageType.Error;
                case PS2ValidationSeverity.Warning:
                    return MessageType.Warning;
                default:
                    return MessageType.Info;
            }
        }

        // ------------------------------------------------------------------ state

        private void RebuildSerializedState()
        {
            _serializedProfile = null;
            _sceneList = null;
            if (_profile == null)
            {
                return;
            }
            _serializedProfile = new SerializedObject(_profile);
            _sceneList = new ReorderableList(
                _serializedProfile, _serializedProfile.FindProperty("scenes"), true, true, true, true);
            _sceneList.drawHeaderCallback = DrawSceneListHeader;
            _sceneList.drawElementCallback = DrawSceneListElement;
        }

        private void DrawSceneListHeader(Rect rect)
        {
            EditorGUI.LabelField(rect, "Scene List");
        }

        private void DrawSceneListElement(Rect rect, int index, bool isActive, bool isFocused)
        {
            SerializedProperty element = _sceneList.serializedProperty.GetArrayElementAtIndex(index);
            rect.y += 2f;
            rect.height = EditorGUIUtility.singleLineHeight;
            EditorGUI.PropertyField(rect, element, GUIContent.none);
        }

        private static string PrefKeyForProject()
        {
            // Keyed per project: EditorPrefs is shared machine-wide.
            return ProfilePrefKeyBase + "." + Application.dataPath.GetHashCode().ToString("x8");
        }

        private void LoadRememberedProfile()
        {
            if (_profile != null)
            {
                return;
            }
            string guid = EditorPrefs.GetString(PrefKeyForProject(), string.Empty);
            if (string.IsNullOrEmpty(guid))
            {
                return;
            }
            string path = AssetDatabase.GUIDToAssetPath(guid);
            if (!string.IsNullOrEmpty(path))
            {
                _profile = AssetDatabase.LoadAssetAtPath<PS2BuildProfile>(path);
            }
        }

        private void RememberProfile()
        {
            string guid = string.Empty;
            if (_profile != null)
            {
                guid = AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(_profile));
            }
            EditorPrefs.SetString(PrefKeyForProject(), guid);
        }
    }
}
