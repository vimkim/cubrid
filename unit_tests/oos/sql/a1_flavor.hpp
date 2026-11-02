// Adversary a1 repro helper: 1 when the engine under test declares heap_recdes_get_var_offset_pair () (PR head),
// 0 when it does not (base dcbf09e4f). Toggled by hand together with `git checkout <sha> -- src/`.
#define A1_HAS_PAIR 1
