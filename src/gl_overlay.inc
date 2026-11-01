// gl_overlay.inc — the FPS counter, the controller's cursor and the mapper's markers, drawn on
// the game's finished frame at its swap. Included twice by gles_bridge.c: on ps5-opengl (desktop
// GL) and on ANGLE (OpenGL ES), its GL calls then ANGLE's. The includer defines OVL_NAME(x) (the
// copy's names), OVL_S (screen pixels per logical pixel: the overlay works in 1920x1080),
// OVL_GLSL_VS / OVL_GLSL_FS (the shaders' first lines) and OVL_DESKTOP.



static void OVL_NAME(ovl_rect)(int x, int y_top, int w, int h, float r, float g, float b) {
    glScissor(x * OVL_S, (OVL_SCREEN_H - y_top - h) * OVL_S, w * OVL_S, h * OVL_S);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static double OVL_NAME(ovl_now)(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// The controller's cursor (unity_host.c): a classic arrow, black edge, white inside.
// Ruffle Flash PS5's pointer (src/ui/gfx.rs pointer()): a white arrow with a dark edge
// and a soft shadow, its tip at the cursor; drawn once into a texture (4x4 samples a
// pixel, as Ruffle's fill_polygon), then put on the frame each swap.
static GLuint OVL_NAME(cur_tex), OVL_NAME(cur_prog), OVL_NAME(cur_vao);
static GLint OVL_NAME(cur_u_rect);

static int OVL_NAME(cursor_setup)(void) {
    if (OVL_NAME(cur_prog)) return 1;
    static float px[CUR_W * CUR_H * 4];
    overlay_cursor_art(px, CUR_W, CUR_H, CUR_O, 1.0f); // overlay_art.c
    static unsigned char rgba[CUR_W * CUR_H * 4];
    for (int i = 0; i < CUR_W * CUR_H; i++) { // premultiplied for the blend
        float a = px[i * 4 + 3];
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (unsigned char)(px[i * 4 + c] * a * 255 + 0.5f);
        rgba[i * 4 + 3] = (unsigned char)(a * 255 + 0.5f);
    }
    glGenTextures(1, &OVL_NAME(cur_tex));
    glBindTexture(GL_TEXTURE_2D, OVL_NAME(cur_tex));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CUR_W, CUR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    static const char *vs =
        OVL_GLSL_VS
        "uniform vec4 rect;\n" // x0, y0, x1, y1 in NDC
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "  uv = vec2(c.x, 1.0 - c.y);\n"
        "  gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        OVL_GLSL_FS
        "uniform sampler2D tex;\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "void main() { color = texture(tex, uv); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    OVL_NAME(cur_prog) = glCreateProgram();
    glAttachShader(OVL_NAME(cur_prog), v);
    glAttachShader(OVL_NAME(cur_prog), f);
    glLinkProgram(OVL_NAME(cur_prog));
    GLint linked = 0;
    glGetProgramiv(OVL_NAME(cur_prog), GL_LINK_STATUS, &linked);
    droid_log("gl: cursor (Ruffle's arrow) ready: %s", linked ? "ok" : "link failed");
    OVL_NAME(cur_u_rect) = glGetUniformLocation(OVL_NAME(cur_prog), "rect");
    glGenVertexArrays(1, &OVL_NAME(cur_vao));
    return linked;
}

// A premultiplied texture on the window at (x0, y0) w x h in the 1080p overlay's pixels
// (framebuffer 0 bound by the caller), the game's state kept: the cursor, the stats panel.
static void OVL_NAME(draw_tex)(GLuint tex, float x0, float y0, float tw, float th) {
    GLint prog, vao, active, tex2d, sampler, viewport[4], bsrc, bdst, bsrca, bdsta, beq;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              cull = glIsEnabled(GL_CULL_FACE), stencil = glIsEnabled(GL_STENCIL_TEST),
              srgb = OVL_DESKTOP ? glIsEnabled(GL_FRAMEBUFFER_SRGB) : GL_FALSE, scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq);
    if (OVL_NAME(cursor_setup)()) {
        glViewport(0, 0, 1920 * OVL_S, OVL_SCREEN_H * OVL_S);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        if (OVL_DESKTOP) glDisable(GL_FRAMEBUFFER_SRGB); // Ruffle's colours as they are
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(OVL_NAME(cur_prog));
        glBindVertexArray(OVL_NAME(cur_vao));
        glBindSampler(0, 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform4f(OVL_NAME(cur_u_rect), x0 / 960.0f - 1, 1 - (y0 + th) / 540.0f, (x0 + tw) / 960.0f - 1, 1 - y0 / 540.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindSampler(0, (GLuint)sampler);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBlendEquation((GLenum)beq);
    glBlendFuncSeparate((GLenum)bsrc, (GLenum)bdst, (GLenum)bsrca, (GLenum)bdsta);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB); // (desktop only: srgb is 0 on ES)
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

static void OVL_NAME(draw_cursor)(void) {
    if (!OVL_NAME(cursor_setup)()) return;
    float cx, cy; // (the phone view: the game's point on the TV)
    view_to_tv(droid_cursor_x, droid_cursor_y, &cx, &cy);
    OVL_NAME(draw_tex)(OVL_NAME(cur_tex), cx - CUR_O, cy - CUR_O, CUR_W, CUR_H);
}

// The stats panel (overlay_stats.c), into its texture when it changed, top left
static GLuint OVL_NAME(st_tex);
static unsigned OVL_NAME(st_ver);
static int OVL_NAME(st_w), OVL_NAME(st_h);
static void OVL_NAME(draw_stats)(void) {
    int w, h;
    unsigned ver;
    const uint8_t *img = overlay_stats_image(app_settings.stats, (float)OVL_S, 1, &w, &h, &ver);
    if (!img || !OVL_NAME(cursor_setup)()) return;
    if (!OVL_NAME(st_tex) || ver != OVL_NAME(st_ver)) {
        GLint tex2d, active, unpack = 0, align = 4, rowlen = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowlen);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (!OVL_NAME(st_tex)) {
            glGenTextures(1, &OVL_NAME(st_tex));
            glBindTexture(GL_TEXTURE_2D, OVL_NAME(st_tex));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, OVL_NAME(st_tex));
        if (w != OVL_NAME(st_w) || h != OVL_NAME(st_h))
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
        OVL_NAME(st_w) = w, OVL_NAME(st_h) = h, OVL_NAME(st_ver) = ver;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
        glActiveTexture((GLenum)active);
    }
    OVL_NAME(draw_tex)(OVL_NAME(st_tex), 24, 24, (float)w / OVL_S, (float)h / OVL_S);
}

// A new GL context (a game that makes one after its loading: Geometry Dash, Unity): our textures,
// program and vertex array were the old one's; their numbers there can be the game's own textures
// (the stats panel went into Geometry Dash's, its picture went mad). Made again in the new one.
static void OVL_NAME(ovl_new_context)(void) {
    OVL_NAME(cur_prog) = OVL_NAME(cur_tex) = OVL_NAME(cur_vao) = 0;
    OVL_NAME(st_tex) = 0;
    OVL_NAME(st_w) = OVL_NAME(st_h) = 0;
    OVL_NAME(st_ver) = 0;
}

// ShotBox: the game's finished picture (before our overlay), read back at the swap
static void OVL_NAME(shot_box)(void) {
    if (!OVL_DESKTOP) return; // (ANGLE: a 4K picture, not read back yet)
    int sec = droid_shot_due();
    if (!sec) return;
    static uint8_t *px;
    if (!px) px = malloc(1920 * 1080 * 4);
    if (!px) return;
    GLint read_fb = 0, pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    droid_shot_write(px, 1920, 1080, 1920 * 4, 0, 1, sec); // GL's rows: bottom first
}

// the D-pad's board finder (dpad_grid.c): when a host asks, the region around the cursor read
// back (the game's picture, before our overlay)
static void OVL_NAME(grid_probe)(void) {
    float cx, cy;
    if (!dpad_grid_wanted(&cx, &cy)) return;
    const int W = OVL_SURF_W, H = OVL_SURF_H, S = OVL_S;
    int n = 640 * S;
    if (n > W) n = W;
    if (n > H) n = H;
    int x0 = (int)(cx * S) - n / 2, y0 = (int)(cy * S) - n / 2; // its top left, rows from the top
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > W - n) x0 = W - n;
    if (y0 > H - n) y0 = H - n;
    static uint8_t *px;
    static int cap;
    if (cap < n * n * 4) {
        free(px);
        px = malloc((size_t)n * n * 4);
        cap = px ? n * n * 4 : 0;
        if (!px) return;
    }
    GLint read_fb = 0, pack = 0, align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x0, H - y0 - n, n, n, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    dpad_grid_analyze(px, n, n, 1, (float)x0 / S, (float)y0 / S, 1.0f / S);
}

static void OVL_NAME(draw_fps_overlay)(void) {
    OVL_NAME(grid_probe)();
    OVL_NAME(shot_box)();
    overlay_stats_tick(); // (every swap: the frame times)
    const int shown = app_settings.stats; // Settings > Performance > Stats overlay: Off / FPS / Full
    if (!shown && !droid_cursor_shown && !droid_mapper_on) return;

    // keep the game's GL state
    GLint fb = 0, box[4];
    GLfloat clear[4];
    GLboolean mask[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (droid_mapper_on) // the mapper's markers: squares with a black border
        for (int k = 0; k < droid_marker_count && k < DROID_MARKERS; k++) {
            const droid_marker *d = &droid_markers[k];
            int side = d->size + 6;
            float mx, my;
            view_to_tv(d->x, d->y, &mx, &my);
            OVL_NAME(ovl_rect)((int)mx - side / 2, (int)my - side / 2, side, side, 0, 0, 0);
            OVL_NAME(ovl_rect)((int)mx - d->size / 2, (int)my - d->size / 2, d->size, d->size, d->rgb[0], d->rgb[1], d->rgb[2]);
        }
    if (shown) OVL_NAME(draw_stats)();               // framebuffer 0 is bound here
    if (droid_cursor_shown) OVL_NAME(draw_cursor)(); // (over the panel)

    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor) glDisable(GL_SCISSOR_TEST);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fb);
}


// ---- the Quick menu (quick_menu.c) at the swap: the game's frame read back once, then frames
// of ours, presented by present(arg): the frosted frame stretched over the screen, the card
// over it (premultiplied, faded by its alpha). The game's GL state kept for after; the last
// frame stays in the back buffer (the frame itself at a resume) for the caller's swap.
static GLuint OVL_NAME(qm_prog), OVL_NAME(qm_vao);
static GLint OVL_NAME(qm_u_rect), OVL_NAME(qm_u_alpha);
static void *OVL_NAME(qm_ctx); // the context they were made in

static int OVL_NAME(qm_setup)(void *ctx) {
    if (OVL_NAME(qm_prog) && OVL_NAME(qm_ctx) == ctx) return 1;
    static const char *vs =
        OVL_GLSL_VS
        "uniform vec4 rect;\n" // x0, y0, x1, y1 in NDC
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "  uv = vec2(c.x, 1.0 - c.y);\n"
        "  gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        OVL_GLSL_FS
        "uniform sampler2D tex;\n"
        "uniform float alpha;\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "void main() { color = texture(tex, uv) * alpha; }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        droid_log("gl: quick menu: its program did not link");
        return 0;
    }
    OVL_NAME(qm_prog) = p;
    OVL_NAME(qm_ctx) = ctx;
    OVL_NAME(qm_u_rect) = glGetUniformLocation(p, "rect");
    OVL_NAME(qm_u_alpha) = glGetUniformLocation(p, "alpha");
    glGenVertexArrays(1, &OVL_NAME(qm_vao));
    return 1;
}

static void OVL_NAME(qm_upload)(GLuint *tex, int *tw, int *th, const uint8_t *img, int w, int h) {
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    if (w != *tw || h != *th) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
    *tw = w, *th = h;
}

static void OVL_NAME(qm_quad)(GLuint tex, float x, float y, float w, float h, float alpha, int W, int H) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform4f(OVL_NAME(qm_u_rect), x / W * 2 - 1, 1 - (y + h) / H * 2, (x + w) / W * 2 - 1, 1 - y / H * 2);
    glUniform1f(OVL_NAME(qm_u_alpha), alpha);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void OVL_NAME(quick_menu_run)(void *ctx, void (*present)(void *), void *arg) {
    const int W = OVL_SURF_W, H = OVL_SURF_H;
    // the game's state
    GLint read_fb, draw_fb, pack, unpack, pack_align, unpack_align, unpack_row, prog, vao, active, tex2d, sampler,
        viewport[4], box[4], bsrc, bdst, bsrca, bdsta, beq;
    GLfloat clear[4];
    GLboolean mask[4], blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
              stencil = glIsEnabled(GL_STENCIL_TEST), scissor = glIsEnabled(GL_SCISSOR_TEST),
              srgb = OVL_DESKTOP ? glIsEnabled(GL_FRAMEBUFFER_SRGB) : GL_FALSE;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);

    // the frame, read back once
    uint8_t *px = malloc((size_t)W * H * 4);
    int ok = 0;
    if (px) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ok = quick_menu_begin(px, W, H, (size_t)W * 4, 0, 1, W, H);
        free(px);
    } else {
        quick_menu_begin(NULL, 0, 0, 0, 0, 0, W, H); // (the request let go)
    }
    int drawn = 0;
    if (ok && OVL_NAME(qm_setup)(ctx)) {
        GLuint back = 0, card = 0;
        int bw0 = 0, bh0 = 0, cw0 = 0, ch0 = 0;
        unsigned bv0 = 0, cv0 = 0;
        int uploads = 0;
        double t_frame = 0, t_draw = 0, t_present = 0; // (the log: where a frame's time goes)
        for (;;) {
            double q0 = OVL_NAME(ovl_now)();
            int more = quick_menu_frame();
            double q1 = OVL_NAME(ovl_now)();
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glViewport(0, 0, W, H);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_SCISSOR_TEST);
            if (OVL_DESKTOP) glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(OVL_NAME(qm_prog));
            glBindVertexArray(OVL_NAME(qm_vao));
            glBindSampler(0, 0);
            int bw, bh, cx, cy, cw, ch;
            unsigned bv, cv;
            float ca;
            const uint8_t *b = quick_menu_backdrop(&bw, &bh, &bv);
            if (b && (bv != bv0 || !back)) OVL_NAME(qm_upload)(&back, &bw0, &bh0, b, bw, bh), bv0 = bv, uploads++;
            if (back) OVL_NAME(qm_quad)(back, 0, 0, (float)W, (float)H, 1, W, H);
            const uint8_t *c = quick_menu_card(&cx, &cy, &cw, &ch, &ca, &cv);
            if (c && (cv != cv0 || !card)) OVL_NAME(qm_upload)(&card, &cw0, &ch0, c, cw, ch), cv0 = cv, uploads++;
            if (card && ca > 0.002f) OVL_NAME(qm_quad)(card, (float)cx, (float)cy, (float)cw, (float)ch, ca, W, H);
            drawn++;
            double q2 = OVL_NAME(ovl_now)();
            if (!more) break;
            present(arg);
            t_frame += q1 - q0, t_draw += q2 - q1, t_present += OVL_NAME(ovl_now)() - q2;
        }
        if (drawn > 1)
            droid_log("gl: quick menu: a frame's time: %.1f ms the menu (with its wait), %.1f ms drawing, %.1f ms the swap; %d uploads",
                      t_frame * 1000 / (drawn - 1), t_draw * 1000 / (drawn - 1), t_present * 1000 / (drawn - 1), uploads);
        if (back) glDeleteTextures(1, &back);
        if (card) glDeleteTextures(1, &card);
    }
    if (ok) droid_log("gl: quick menu: %d frames", drawn);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    glBindSampler(0, (GLuint)sampler);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    glBlendEquation((GLenum)beq);
    glBlendFuncSeparate((GLenum)bsrc, (GLenum)bdst, (GLenum)bsrca, (GLenum)bdsta);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (ok) quick_menu_end(); // (Close Game: the app closes from here)
}
