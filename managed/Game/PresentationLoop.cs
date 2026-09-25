using System;
using System.Collections.Generic;
using UnityEngine.LowLevel;
using UnityEngine.PlayerLoop;

namespace DSPAAMod.Game
{
    // Edits the CURRENT loop, preserving every other mod's systems. The unique
    // marker types also permit precise removal without restoring an old snapshot.
    internal sealed class PresentationLoop : IDisposable
    {
        private struct BeforeInput { }
        private struct BeforeRender { }
        private readonly PlayerLoopSystem.UpdateFunction begin, render;
        private bool installed;
        public PresentationLoop(Action beforeInput, Action beforeRender)
        {
            begin = () => beforeInput(); render = () => beforeRender();
            var root = PlayerLoop.GetCurrentPlayerLoop();
            if (Contains(root, typeof(BeforeInput)) || Contains(root, typeof(BeforeRender)))
                throw new InvalidOperationException("A presentation loop owner is already installed.");
            if (root.subSystemList == null || root.updateDelegate != null || root.updateFunction != IntPtr.Zero)
                throw new NotSupportedException("The current Unity root loop cannot guarantee a before-input entry.");
            int boundaries = InsertBefore(ref root, typeof(PostLateUpdate.UpdateAllRenderers),
                new PlayerLoopSystem { type = typeof(BeforeRender), updateDelegate = render });
            if (boundaries != 1) throw new NotSupportedException("The current Unity loop has no unique renderer-submission boundary.");
            var children = new List<PlayerLoopSystem>(root.subSystemList);
            children.Insert(0, new PlayerLoopSystem { type = typeof(BeforeInput), updateDelegate = begin });
            root.subSystemList = children.ToArray();
            PlayerLoop.SetPlayerLoop(root); installed = true;
        }
        private static bool Contains(PlayerLoopSystem system, Type type)
        {
            if (system.type == type) return true;
            if (system.subSystemList != null)
                foreach (var child in system.subSystemList) if (Contains(child, type)) return true;
            return false;
        }
        private static int InsertBefore(ref PlayerLoopSystem system, Type type, PlayerLoopSystem insertion)
        {
            if (system.subSystemList == null) return 0;
            int count = 0;
            var children = new List<PlayerLoopSystem>();
            foreach (var original in system.subSystemList)
            {
                var child = original;
                if (child.type == type) { children.Add(insertion); ++count; }
                count += InsertBefore(ref child, type, insertion);
                children.Add(child);
            }
            system.subSystemList = children.ToArray();
            return count;
        }
        private static void Remove(ref PlayerLoopSystem system)
        {
            if (system.subSystemList == null) return;
            var children = new List<PlayerLoopSystem>();
            foreach (var original in system.subSystemList)
            {
                if (original.type == typeof(BeforeInput) || original.type == typeof(BeforeRender)) continue;
                var child = original; Remove(ref child); children.Add(child);
            }
            system.subSystemList = children.ToArray();
        }
        public void Dispose()
        {
            if (!installed) return;
            var root = PlayerLoop.GetCurrentPlayerLoop(); Remove(ref root); PlayerLoop.SetPlayerLoop(root); installed = false;
        }
    }
}
