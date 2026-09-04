* ============================================================================
 *  T I T A N   K E R N E L   v3.0  --  "Glass Edition"
 *  A single-file, from-scratch, protected-mode x86 kernel with its own GUI.
 *
 *  What changed from v2 -> v3:
 *    - PERFORMANCE FIX: v2 drew every shape directly to video memory, one
 *      pixel at a time, through a bounds-checked function call. On a
 *      1920x1080 framebuffer that is ~1,000,000 checked function calls per
 *      full redraw, and a redraw fired on *every single mouse packet* --
 *      that's what made the cursor feel like it was sticking/lagging.
 *      v3 draws into an off-screen backbuffer using tight `rep stosl`
 *      (fill) and `rep movsl` (blit) loops, and only presents the finished
 *      frame to video memory once per redraw. This is a completely
 *      different order of magnitude of speed, and also removes the
 *      flicker/tearing that made the UI feel rough.
 *    - VISUAL REFRESH: rounded-corner translucent ("glass") windows that
 *      genuinely blend with the desktop gradient behind them, soft layered
 *      drop shadows, a glowing accent taskbar, hover/press glow on buttons,
 *      a blinking text cursor in the terminal, and a smooth vertical
 *      gradient desktop instead of flat color bands.
 *
 *  
 *
 *  Build: see build.sh in the same folder.
 * ==========================================================================*/
