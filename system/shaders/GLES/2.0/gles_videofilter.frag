#version 100

precision mediump float;

uniform sampler2D img;
varying vec2 cord;

void main()
{
  gl_FragColor = texture2D(img, cord);
#if defined(KODI_CLAMP_OUTPUT)
  gl_FragColor = clamp(gl_FragColor, 0.0, 1.0);
#endif
}
