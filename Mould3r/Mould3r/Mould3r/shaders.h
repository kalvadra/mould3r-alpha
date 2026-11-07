#pragma once
class shaders
{
public:
    //Define basic vertex shader
    const char* vsSrc = R"GLSL(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    layout (location = 1) in vec3 aColor;

    out vec3 vColor;

    uniform mat4 uModel;
    uniform mat4 uView;
    uniform mat4 uProj;

    void main() {
    vColor = aColor;
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
    )GLSL";

    //Define basic fragment shader
    const char* fsSrc = R"GLSL(
        #version 330 core
        in vec3 vColor;
        out vec4 FragColor;
        void main() {
            FragColor = vec4(vColor, 1.0);
        }
    )GLSL";

    //Define expanded vertex shader for more advanced lighting
    const char* vsLit = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec3 aNormal;

        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProj;

        out vec3 vWorldPos;
        out vec3 vWorldNormal;

        void main()
        {
            vec4 worldPos = uModel * vec4(aPos, 1.0);
            vWorldPos = worldPos.xyz;

            // Correct normal transform (handles non-uniform scaling)
            mat3 normalMat = transpose(inverse(mat3(uModel)));
            vWorldNormal = normalize(normalMat * aNormal);

            gl_Position = uProj * uView * worldPos;
        }
        )GLSL";

    //Define expanded fragment shader for more advanced lighting.
    //
    // Hybrid lighting rig (Tier 0):
    //   * KEY   — one world-anchored directional light. Because it is fixed in
    //             world space, "up" stays meaningful, so shading still encodes
    //             orientation relative to the parting plane (draft / demould
    //             reads). uLightDir points FROM the surface TOWARD the light.
    //   * FILL  — one camera-relative directional light (uFillDir is derived
    //             from the view matrix on the CPU). It follows the camera so
    //             the near face never orbits into darkness, but it is dim and
    //             diffuse-only so it lifts shadows without flattening form.
    //   * AMBIENT — hemisphere term: cool sky from above, warm bounce from
    //             below, instead of one flat constant. uAmbient is its strength.
    //
    // Shading is done in linear space (the base colour is sRGB-decoded on the
    // way in and the result is re-encoded on the way out) because the default
    // framebuffer is not sRGB. uEmissive drives a flat/unlit path used by the
    // separation-test debug highlights, whose look must not change.
    const char* fsLit = R"GLSL(
        #version 330 core
        in vec3 vWorldPos;
        in vec3 vWorldNormal;
        out vec4 FragColor;

        uniform vec3 uCameraPos;

        uniform vec3 uLightDir;    // KEY: surface->light (world), normalized
        uniform vec3 uLightColor;
        uniform vec3 uFillDir;     // FILL: surface->light (camera-relative), normalized
        uniform vec3 uFillColor;

        uniform vec3 uSkyColor;    // hemisphere ambient, up
        uniform vec3 uGroundColor; // hemisphere ambient, down

        uniform vec3 uBaseColor;
        uniform float uAlpha;

        uniform float uAmbient;    // hemisphere ambient strength (e.g. 0.25)
        uniform float uDiffuse;    // e.g. 0.85
        uniform float uSpecular;   // key specular strength (e.g. 0.20)
        uniform float uShininess;  // e.g. 64

        uniform float uEmissive;   // >0.5 => flat unlit base colour (debug)

        uniform vec3  uRimColor;    // Fresnel silhouette rim tint
        uniform float uRimStrength; // rim intensity (e.g. 0.22)
        uniform float uRimPower;    // rim tightness (e.g. 3.5)

        // Peak-normalized GGX (Trowbridge-Reitz) specular distribution. Peaks
        // at 1.0 when NdotH == 1, so it drops straight into the existing
        // uSpecular scale with no re-tuning, but carries the GGX shape: a
        // tighter highlight core and a longer, softer tail than Blinn-Phong.
        float ggxSpec(float NdotH, float rough)
        {
            float a  = rough * rough;
            float a2 = a * a;
            float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
            return (a2 * a2) / max(d * d, 1e-8);
        }

        void main()
        {
            // Deliberately flat/unlit path — separation-test highlights. Keep
            // the exact pre-existing look (authored display-space colour, no
            // gamma round-trip), so this stays visually identical.
            if (uEmissive > 0.5)
            {
                FragColor = vec4(uBaseColor, uAlpha);
                return;
            }

            vec3 N = normalize(vWorldNormal);
            vec3 V = normalize(uCameraPos - vWorldPos);

            // sRGB-decode the base colour so lighting composes in linear space.
            vec3 base = pow(uBaseColor, vec3(2.2));

            // Hemisphere ambient: 1 = up-facing (sky), 0 = down-facing (ground).
            float hemi = 0.5 * N.y + 0.5;
            vec3 ambient = mix(uGroundColor, uSkyColor, hemi) * uAmbient;

            // Roughness from the legacy shininess knob so per-pass tuning
            // (body 64, features 48) still applies unchanged.
            float rough = clamp(sqrt(2.0 / (uShininess + 2.0)), 0.045, 1.0);

            // Key light — diffuse + GGX specular. Specular is scaled by NdotL
            // so it never appears on faces turned away from the key.
            vec3 Lk = normalize(uLightDir);
            float ndlK = max(dot(N, Lk), 0.0);
            vec3 Hk = normalize(Lk + V);
            float NdotHk = max(dot(N, Hk), 0.0);
            float specK = ggxSpec(NdotHk, rough) * ndlK;

            // Fill light — diffuse only, dim; keeps the viewer-facing side alive.
            vec3 Lf = normalize(uFillDir);
            float ndlF = max(dot(N, Lf), 0.0);

            // Diffuse + ambient are tinted by the surface colour.
            vec3 diffuse = uLightColor * (uDiffuse * ndlK)
                         + uFillColor  * (uDiffuse * ndlF);
            vec3 color = base * (ambient + diffuse);

            // Key specular added untinted (dielectric highlight).
            color += uLightColor * (uSpecular * specK);

            // Fresnel silhouette rim — a view-dependent edge highlight that
            // outlines the model from any angle, independent of the key
            // direction. This is the "CAD inspection" edge pop; kept subtle so
            // it reads as edge definition rather than emission.
            float rim = pow(1.0 - max(dot(N, V), 0.0), uRimPower) * uRimStrength;
            color += uRimColor * rim;

            // Linear -> sRGB for display.
            color = pow(color, vec3(1.0 / 2.2));

            FragColor = vec4(color, uAlpha);
        }
        )GLSL";

    const char* vsPick = R"GLSL(
        #version 330 core
        layout(location=0) in vec3 aPos;

        uniform mat4 uMVP;

        void main() {
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
        )GLSL";

    const char* fsPick = R"GLSL(
        #version 330 core
        layout(location=0) out uint outId;

        uniform uint uObjectId;

        void main() {
            outId = uObjectId;
        }
        )GLSL";

    const char* vsFullscreen = R"GLSL(
    #version 330 core

    out vec2 vUV;

    // Fullscreen triangle via gl_VertexID (no VBO needed)
    void main()
    {
        vec2 pos;
        if (gl_VertexID == 0) { pos = vec2(-1.0, -1.0); vUV = vec2(0.0, 0.0); }
        if (gl_VertexID == 1) { pos = vec2( 3.0, -1.0); vUV = vec2(2.0, 0.0); }
        if (gl_VertexID == 2) { pos = vec2(-1.0,  3.0); vUV = vec2(0.0, 2.0); }

        gl_Position = vec4(pos, 0.0, 1.0);
    }
)GLSL";

    const char* fsOutline = R"GLSL(
    #version 330 core

    in vec2 vUV;
    out vec4 outColor;

    uniform usampler2D uIdTex;      // GL_R32UI
    uniform uint       uTargetId;   // 1 for your single model
    uniform ivec2      uTexSize;    // (w,h)
    uniform float      uAlpha;      // 0..1
    uniform int        uThickness;  // pixels (1..4)

    void main()
    {
        // Convert UV->pixel coords (clamp inside)
        ivec2 p = ivec2(vUV * vec2(uTexSize));
        p = clamp(p, ivec2(0), uTexSize - ivec2(1));

        uint c = texelFetch(uIdTex, p, 0).r;

        // Sample neighbors at +/- thickness
        int t = max(uThickness, 1);

        uint l = texelFetch(uIdTex, clamp(p + ivec2(-t,  0), ivec2(0), uTexSize - ivec2(1)), 0).r;
        uint r = texelFetch(uIdTex, clamp(p + ivec2( t,  0), ivec2(0), uTexSize - ivec2(1)), 0).r;
        uint d = texelFetch(uIdTex, clamp(p + ivec2( 0, -t), ivec2(0), uTexSize - ivec2(1)), 0).r;
        uint u = texelFetch(uIdTex, clamp(p + ivec2( 0,  t), ivec2(0), uTexSize - ivec2(1)), 0).r;

        bool centerIs = (c == uTargetId);
        bool anyDiff  = (l != c) || (r != c) || (d != c) || (u != c);

        // Silhouette edge: boundary between target and non-target
        bool edge = false;
        if (centerIs)
        {
            edge = anyDiff; // leaving the object
        }
        else
        {
            // entering the object
            edge = (l == uTargetId) || (r == uTargetId) || (d == uTargetId) || (u == uTargetId);
        }

        if (!edge)
            discard;

        // Outline color (tweak here)
        vec3 outlineRGB = vec3(1.0, 0.75, 0.2);
        outColor = vec4(outlineRGB, uAlpha);
    }
)GLSL";

    // Flat (unlit) shaders — used for vent path lines
    const char* vsFlat = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uVP;   // proj * view (paths are already in world space)
        void main()
        {
            gl_Position = uVP * vec4(aPos, 1.0);
        }
        )GLSL";

    const char* fsFlat = R"GLSL(
        #version 330 core
        out vec4 FragColor;
        uniform vec4 uColor;
        void main()
        {
            FragColor = uColor;
        }
        )GLSL";

    // Wide lines (vsFlat + gsWideLine + fsFlat). glLineWidth > 1 is not
    // available in a core-profile context on macOS (and is deprecated
    // everywhere in core), so each segment is expanded here into a
    // screen-space quad uWidth pixels wide, with square caps so the joints of
    // strips and loops have no gaps. Accepts GL_LINES, GL_LINE_STRIP and
    // GL_LINE_LOOP draws. Depth comes from the original endpoints, so depth
    // testing behaves as it did with real lines.
    const char* gsWideLine = R"GLSL(
        #version 330 core
        layout(lines) in;
        layout(triangle_strip, max_vertices = 4) out;
        uniform vec2  uViewport;   // framebuffer size, pixels (from GL_VIEWPORT)
        uniform float uWidth;      // line width, framebuffer pixels
        void main()
        {
            vec4 p0 = gl_in[0].gl_Position;
            vec4 p1 = gl_in[1].gl_Position;

            // Clip against the near plane (z >= -w) before dividing by w, so a
            // segment that passes behind the camera can't flip or explode.
            float d0 = p0.z + p0.w;
            float d1 = p1.z + p1.w;
            if (d0 < 0.0 && d1 < 0.0) return;
            if (d0 < 0.0)      p0 = mix(p0, p1, d0 / (d0 - d1));
            else if (d1 < 0.0) p1 = mix(p1, p0, d1 / (d1 - d0));

            // Direction and normal in pixel space.
            vec2 s0 = (p0.xy / p0.w) * 0.5 * uViewport;
            vec2 s1 = (p1.xy / p1.w) * 0.5 * uViewport;
            vec2 dir = s1 - s0;
            float len = length(dir);
            dir = (len > 1e-4) ? dir / len : vec2(1.0, 0.0);
            vec2 nrm = vec2(-dir.y, dir.x);

            // Half-width offsets, converted from pixels back to clip space
            // (scaled by w so they survive the perspective divide).
            vec2 toNdc = 2.0 / uViewport;
            vec2 offN = nrm * (0.5 * uWidth) * toNdc;
            vec2 offT = dir * (0.5 * uWidth) * toNdc;

            gl_Position = vec4(p0.xy + (-offT + offN) * p0.w, p0.zw); EmitVertex();
            gl_Position = vec4(p0.xy + (-offT - offN) * p0.w, p0.zw); EmitVertex();
            gl_Position = vec4(p1.xy + ( offT + offN) * p1.w, p1.zw); EmitVertex();
            gl_Position = vec4(p1.xy + ( offT - offN) * p1.w, p1.zw); EmitVertex();
            EndPrimitive();
        }
        )GLSL";

    // Sized points (vsFlatPoint + fsFlat). Point size comes from the shader
    // (GL_PROGRAM_POINT_SIZE) rather than glPointSize, which works the same in
    // every core-profile implementation.
    const char* vsFlatPoint = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        uniform mat4  uVP;
        uniform float uPointSize;  // framebuffer pixels
        void main()
        {
            gl_Position = uVP * vec4(aPos, 1.0);
            gl_PointSize = uPointSize;
        }
        )GLSL";

};