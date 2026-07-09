vec3 a_position   : POSITION;
vec3 a_normal     : NORMAL;
vec2 a_texcoord0  : TEXCOORD0;
vec2 a_texcoord1  : TEXCOORD1;
vec4 a_color0     : COLOR0;

vec3 v_normal        : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0     : TEXCOORD0 = vec2(0.0, 0.0);
vec2 v_texcoord1     : TEXCOORD6 = vec2(0.0, 0.0);
vec4 v_color0        : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec4 v_worldPos      : TEXCOORD1 = vec4(0.0, 0.0, 0.0, 1.0);
vec3 v_reflectVector : TEXCOORD2 = vec3(0.0, 0.0, 1.0);
float v_vdn          : TEXCOORD3 = 1.0;
vec4 v_diffuse       : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_specular      : TEXCOORD5 = vec4(0.0, 0.0, 0.0, 1.0);
float v_vertPos      : TEXCOORD7 = 0.0;
vec3 v_objectPos     : TEXCOORD8 = vec3(0.0, 0.0, 0.0);
vec3 v_diffuseExtra  : TEXCOORD9 = vec3(0.0, 0.0, 0.0);
