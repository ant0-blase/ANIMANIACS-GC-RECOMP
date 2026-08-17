# Animaniacs source origin

This independent, flattened framework snapshot was created on 2026-08-11
from these source revisions:

- ModernGekko: `d4c030ec7f5b`
- RecompCore runtime: `eeaad721ded6`
- DolRecomp: `e7d677ab58ee`

All initialized external dependency contents were copied into this tree. Git
submodule metadata, pre-migration build directories, and loose `*.pre-*`
backup files were deliberately excluded. This repository has no remote and no
live relationship to another game project.

Animaniacs-local divergence begins with a four-job cap in the port tool so its
module generator and compiler respect this project's resource limit. Future
generic fixes must be exported and applied to another project manually; this
tree is never synchronized automatically.
