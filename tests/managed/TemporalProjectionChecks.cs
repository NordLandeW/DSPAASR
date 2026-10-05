using System;
using DSPAAMod.Core;
using DSPAAMod.Game;
using UnityEngine;

internal static partial class Program
{
    private static void TemporalProjectionChecks()
    {
        // Catch loss of the blueprint lens shift, jitter accumulation, wrong
        // render-size normalization, and default resets breaking next-frame
        // picking. These are the production matrix/scope operations; Unity's
        // renderer and native Camera methods are not run by this test host.
        var perspective = new Matrix4x4 {
            m00 = 1.3f, m11 = 2.1f, m02 = 0.203125f, m12 = -0.075f,
            m20 = 0.02f, m21 = -0.03f, m22 = -1.01f, m23 = -0.61f, m32 = -1f
        };
        var orthographic = new Matrix4x4 {
            m00 = 0.2f, m11 = 0.3f, m22 = -0.02f, m33 = 1f, m03 = -0.18f, m13 = 0.06f, m23 = -0.5f
        };
        var points = new[] { new Vector4(0, 0, -4, 1), new Vector4(1.3f, -0.7f, -8, 1), new Vector4(-0.4f, 0.8f, -2, 1) };
        foreach (bool ortho in new[] { false, true })
        foreach (var size in new[] { (3840, 2160), (2560, 1440), (1280, 720) })
        {
            Matrix4x4 original = ortho ? orthographic : perspective;
            var scope = new TemporalProjection();
            scope.Begin(original, ortho, size.Item1, size.Item2, true);
            for (uint frame = 0; frame < Jitter.PhaseCount; ++frame)
            {
                var sample = Jitter.ForFrame(frame);
                Matrix4x4 raster = scope.JitteredMatrix(new Vector2(sample.X, sample.Y));
                Require(scope.Original.Equals(original), "Jitter mutated the saved game projection");
                for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                {
                    if (row < 2 && column == (ortho ? 3 : 2)) continue;
                    Require(raster[row, column] == original[row, column], "Jitter overwrote an unrelated projection coefficient");
                }
                foreach (var point in points)
                {
                    Vector4 before = original * point, after = raster * point;
                    double dx = ((double)after.x / after.w - (double)before.x / before.w) * size.Item1 * 0.5;
                    double dy = ((double)after.y / after.w - (double)before.y / before.w) * size.Item2 * 0.5;
                    Require(Math.Abs(dx + sample.X) < 0.001 && Math.Abs(dy + sample.Y) < 0.001,
                        "Raster jitter lost the game lens shift, accumulated, or used output dimensions instead of render dimensions");
                }
                Require(scope.TryReset(raster, out var restored) && restored.Equals(original),
                    "Post-render reset did not restore the exact projection used for world-space interaction");
                Require(scope.TryReset(restored, out var again) && again.Equals(original),
                    "An idempotent post-render reset fell through to Unity's centered default");
                Require(!scope.TryRestore(raster, out var later) && later.Equals(raster),
                    "An ended render stole a subsequent equal-valued game projection");
            }
            // Image effects may release their other overrides before the native
            // post-render reset. That later reset must still retain the game lens.
            var applied = scope.JitteredMatrix(new Vector2(0.3f, -0.2f));
            Require(scope.TryRestore(applied, out var imageRestored) && imageRestored.Equals(original),
                "Image-effect cleanup failed to remove jitter");
            Require(scope.TryReset(imageRestored, out var postRestored) && postRestored.Equals(original),
                "Image-effect cleanup discarded the later reset's ownership");
            scope.Clear();
            Require(!scope.TryReset(applied, out _) && !scope.TryRestore(applied, out _),
                "A cleared/disabled scope retained an old render's projection");
        }

        var game = new TemporalProjection();
        Require(!game.TryReset(perspective, out _) && !game.TryRestore(perspective, out _),
            "An inactive AA camera intercepted the original reset");
        game.Begin(perspective, false, 3840, 2160, true);
        var custom = perspective;
        custom.m02 = 0.28f; custom.m03 = 0.015f;
        game.Observe(custom); // An existing custom jitter delegate owns its output.
        Require(game.TryRestore(custom, out var clean) && clean.Equals(perspective),
            "Custom jitter delegate output was not restored to its input projection");
        game.Begin(perspective, false, 3840, 2160, true);
        game.Observe(custom);
        var foreign = custom;
        foreign.m02 += 0.000001f;
        Require(!game.TryRestore(foreign, out var kept) && kept.Equals(foreign) &&
            game.TryReset(foreign, out kept) && kept.Equals(foreign),
            "Cleanup overwrote another component's subpixel projection change");
        game.Begin(perspective, false, 3840, 2160, true);
        game.Observe(custom);

        var menu = new TemporalProjection();
        menu.Begin(perspective, false, 1920, 1080, false);
        var menuRaster = menu.JitteredMatrix(new Vector2(0.2f, -0.3f));
        Require(menu.TryRestore(menuRaster, out clean) && clean.Equals(perspective) && !menu.TryReset(clean, out _),
            "A non-game camera failed to clean up before handing projection control back to Unity");
        menu.Begin(perspective, false, 1920, 1080, false);
        var automatic = perspective;
        automatic.m02 = automatic.m12 = 0;
        menu.Observe(automatic); // A custom delegate can return the default lens.
        Require(!menu.TryReset(automatic, out _) && !menu.TryRestore(automatic, out clean) && clean.Equals(automatic),
            "Cleanup reclaimed a non-game camera after its automatic projection reset");
        Require(game.TryReset(custom, out clean) && clean.Equals(perspective), "Another camera replaced the game's snapshot");

        // Enter/leave blueprint mode and change FOV/aspect between renders.
        // A new scope must restore the new projection, never the previous one.
        var next = perspective;
        next.m00 = 1.7f; next.m11 = 2.4f; next.m02 = 0;
        game.Clear();
        game.Begin(next, false, 1920, 1080, true);
        var nextRaster = game.JitteredMatrix(new Vector2(-0.25f, 0.4f));
        Require(game.TryReset(nextRaster, out clean) && clean.Equals(next),
            "A camera transition restored the preceding frame's blueprint/FOV/aspect");
        Console.WriteLine("Temporal projection lens shifts, raster jitter and reset ownership passed.");
    }
}
