# UltimateXR hand assets

The hand geometry and source mask textures used to build these runtime assets
come from [VRMADA/ultimatexr-unity](https://github.com/VRMADA/ultimatexr-unity),
commit `985f20c`.

Used source assets:

- `Runtime/Art/Avatars/BigHands/BigHandLeftGeo.fbx`
- `Runtime/Art/Avatars/BigHands/BigHandRightGeo.fbx`
- `Runtime/Art/Avatars/BigHands/HandPoses/Default.asset`
- `Runtime/Art/Avatars/BigHands/HandPoses/Grab.asset`
- `Runtime/Art/Avatars/BigHands/HandPoses/Wave.asset`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask1.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask2.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask3.png`

The FBX meshes are converted offline to the `UXRH` runtime format. The open,
grip, trigger and combined poses are baked from UltimateXR's authored finger
poses. `BigHandsAlbedo.png` is an offline Type 3 skin-colour approximation made
from the original masks; it does not include Unity or Shader Graph code.

`BigHandLeftPalm.uxrh` and `BigHandRightPalm.uxrh` are basketball-only meshes
with UltimateXR's authored `Wave` pose baked as the open-palm pose. They do not
replace the ordinary controller hand meshes. Rebuild them with
`tools/build_ultimatexr_palm_hands.py` after converting the official BigHands
FBX files to embedded glTF.

UltimateXR is distributed under the MIT License. See
`ULTIMATEXR_LICENSE.txt` in this directory.

The runtime texture is the raw RGBA8 byte stream of `BigHandsAlbedo.png`:

```python
from PIL import Image
Image.open("BigHandsAlbedo.png").convert("RGBA").tobytes()
```

Expected `BigHandsAlbedo.rgba` SHA-256:
`46BA8B2F08CE1CA420537E060F64BE139DD39A22357D4EA5B9B7B69DA2BA2AD4`.
