# Vendored source

This directory contains a compact, reliable source snapshot needed to build
Mesh Compare's viewport and mesh import stack without a sibling checkout or a
network download.

## Provenance

- Customized MeshLab source:
  `f728ecfd43a4fa1b78c50109b1055f049a8baa32`
- VCGLib source:
  `67ce2cc271e5ec95df56cca00de34c6c097f381b`

The snapshot was exported from those exact Git commits. Unrelated MeshLab
applications and plugins, VCGLib's top-level examples/documentation/images,
build products, download caches, and Git metadata are intentionally excluded.

The retained source is limited to:

- MeshLab `common`, `io_base`, and their CMake support;
- VCGLib headers and wrappers, including OpenFBX and bundled Eigen 3.4.0
  headers;
- bundled GLEW 2.2.0 and EasyExif 1.0.

The local copies of `glew.cmake` and `easyexif.cmake` remove upstream download
fallbacks. A missing vendored file is therefore a configuration error rather
than a network operation.

## Licenses

- MeshLab: `meshlab/LICENSE.txt`
- VCGLib: `meshlab/src/vcglib/LICENSE.txt`
- Eigen: `meshlab/src/vcglib/eigenlib/COPYING.MPL2`
- OpenFBX: `meshlab/src/vcglib/wrap/openfbx/LICENSE`
- GLEW: `meshlab/src/external/glew-2.2.0/LICENSE.txt`
- EasyExif: `meshlab/src/external/easyexif-1.0/LICENSE`

Preserve the provenance, license files, and the two fail-closed OpenGL safety
changes when updating this snapshot.
