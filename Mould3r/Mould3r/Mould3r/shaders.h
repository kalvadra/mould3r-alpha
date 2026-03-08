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

        //Define expanded fragment shader for more advanced lighting
        const char* fsLit = R"GLSL(
        #version 330 core
        in vec3 vWorldPos;
        in vec3 vWorldNormal;
        out vec4 FragColor;

        uniform vec3 uCameraPos;

        uniform vec3 uLightDir;   // direction *toward* surface (world), normalized
        uniform vec3 uLightColor;
        uniform vec3 uBaseColor;

        uniform float uAmbient;   // e.g. 0.25
        uniform float uDiffuse;   // e.g. 0.85
        uniform float uSpecular;  // e.g. 0.20
        uniform float uShininess; // e.g. 64

        void main()
        {
            vec3 N = normalize(vWorldNormal);
            vec3 L = normalize(uLightDir);
            vec3 V = normalize(uCameraPos - vWorldPos);

            float ndotl = max(dot(N, L), 0.0);

            // Blinn-Phong specular
            vec3 H = normalize(L + V);
            float spec = pow(max(dot(N, H), 0.0), uShininess) * step(0.0, ndotl);

            vec3 color = uBaseColor * (uAmbient + uDiffuse * ndotl) * uLightColor;
            color += uLightColor * (uSpecular * spec);

            FragColor = vec4(color, 1.0);
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

};