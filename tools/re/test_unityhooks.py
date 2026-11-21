"""unityhooks.py's checks of what the runtime's jump may replace, on hand-assembled instructions
(no Unity code): python3 -m unittest discover -s tools/re -p 'test_*.py'"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unityhooks  # noqa: E402


class Movable(unittest.TestCase):
    def test_arm(self):
        ok = [0xe92d4070,   # push {r4, r5, r6, lr}
              0xe1a04001,   # mov r4, r1
              0xe24dd008,   # sub sp, sp, #8
              0xe3500003,   # cmp r0, #3
              0x0a000004]   # beq (rewritten by the runtime)
        bad = [0xe59f0010,  # ldr r0, [pc, #16]
               0xe08f0000,  # add r0, pc, r0
               0xeb000010,  # bl
               0xe12fff1e,  # bx lr
               0xfa000001,  # blx imm
               0xe8bd8070]  # pop {r4, r5, r6, pc}
        for w in ok:
            self.assertTrue(unityhooks.arm_movable(w), hex(w))
        for w in bad:
            self.assertFalse(unityhooks.arm_movable(w), hex(w))

    def test_a64(self):
        ok = [0xa9be57fe,   # stp x30, x21, [sp, #-0x20]!
              0xa9014ff4,   # stp x20, x19, [sp, #0x10]
              0x71000c1f,   # cmp w0, #3
              0x2a0103f3,   # mov w19, w1
              # (the runtime rewrites these for their new place)
              0xf0ff9409,   # adrp x9, ...
              0x540003a0,   # b.eq
              0x94000010,   # bl
              0x14000011,   # b
              0x36000189,   # tbz
              0xb4000040]   # cbz x0
        bad = [0x58000050,  # ldr x16, #8 (literal)
               0xd65f03c0,  # ret
               0xd61f0200,  # br x16
               0x91000210]  # add x16, x16, #0 (writes x16)
        for w in ok:
            self.assertTrue(unityhooks.a64_movable(w), hex(w))
        for w in bad:
            self.assertFalse(unityhooks.a64_movable(w), hex(w))


class Thumb(unittest.TestCase):
    # a validate's entry: cmp r0, #0; itt eq; moveq r0, #0x24; bxeq lr; cmp r1, #0; itt eq; ...
    VALIDATE = struct.pack('<8H', 0x2800, 0xbf04, 0x2024, 0x4770, 0x2900, 0xbf04, 0x2025, 0x4770)

    def test_entry(self):
        self.assertTrue(unityhooks.thumb_entry_ok(self.VALIDATE, 8))
        self.assertTrue(unityhooks.thumb_entry_ok(self.VALIDATE, 10))
        self.assertFalse(unityhooks.thumb_entry_ok(self.VALIDATE, 12))        # an IT block cut in two
        # push {r4-r7, lr}; add r7, sp, #12; str r8, [sp, #-4]!: 8 bytes, the last one 32-bit
        self.assertTrue(unityhooks.thumb_entry_ok(struct.pack('<4H', 0xb5f0, 0xaf03, 0xf84d, 0x8d04), 8))
        # push {r7, lr}; mov r7, sp; sub sp, #8; add r1, sp, #4; bl ...: the bl cannot be moved
        wrapper = struct.pack('<6H', 0xb580, 0x466f, 0xb082, 0xa901, 0xf7b2, 0xfd85)
        self.assertTrue(unityhooks.thumb_entry_ok(wrapper, 8))
        self.assertFalse(unityhooks.thumb_entry_ok(wrapper, 10))              # half of the bl

    def test_movable(self):
        ok = [(0x2800, None), (0xb5f0, None), (0x4770, None), (0xbd80, None), (0x466f, None), (0xe92d, 0x41f0)]
        bad = [(0x4800, None),              # ldr r0, [pc, #0]
               (0xa000, None),              # adr r0
               (0x4478, None),              # add r0, pc
               (0xd001, None),              # beq
               (0xe7fe, None),              # b
               (0xb108, None),              # cbz r0
               (0x4780, None),              # blx r0
               (0xf7b2, 0xfd85),            # bl
               (0xf8df, 0xf000)]            # ldr.w pc, [pc]
        for h, h2 in ok:
            self.assertTrue(unityhooks.thumb_movable(h, h2), hex(h))
        for h, h2 in bad:
            self.assertFalse(unityhooks.thumb_movable(h, h2), hex(h))

    def test_targets(self):
        # at 0x1257d46: bl 0x120a854 (f7b2 fd85); at 0x10: beq to 0x14 (d000); b to 0x10 (e7fe at 0x12 -> 0x12)
        self.assertEqual(list(unityhooks.thumb_targets(struct.pack('<2H', 0xf7b2, 0xfd85), 0x1257d46)),
                         [(0x1257d46, 0x120a854)])
        self.assertEqual(list(unityhooks.thumb_targets(struct.pack('<2H', 0xd000, 0xe7fe), 0x10)), [(0x10, 0x14), (0x12, 0x12)])
        self.assertEqual(unityhooks.branches_into(struct.pack('<2H', 0xd000, 0xe7fe), 0x10, 't', 0x12, 0x14), [0x12])


class BranchesInto(unittest.TestCase):
    def test_a64_loop_into_entry(self):
        # entry at 0x1000: 4 instructions, then a b.ne back to 0x1008 (inside the 16 bytes the jump takes)
        words = [0xa9be57fe, 0xa9014ff4, 0x71000c1f, 0x2a0103f3]
        bne_back = 0x54000001 | (((0x1008 - 0x1010) // 4 & 0x7ffff) << 5)
        code = struct.pack('<5I', *words, bne_back)
        self.assertEqual(unityhooks.branches_into(code, 0x1000, 'x', 0x1004, 0x1010), [0x1010])
        code = struct.pack('<5I', *words, 0x54000001 | (((0x1014 - 0x1010) // 4) << 5))
        self.assertEqual(unityhooks.branches_into(code, 0x1000, 'x', 0x1004, 0x1010), [])

    def test_arm_branch_to_second_instruction(self):
        # b to 0x2004 from 0x200c: pc + 8 + imm*4 = 0x2004 -> imm = -4
        b = 0xea000000 | ((-4) & 0xffffff)
        code = struct.pack('<4I', 0xe92d4070, 0xe1a04001, 0xe3500003, b)
        self.assertEqual(unityhooks.branches_into(code, 0x2000, 'a', 0x2004, 0x2008), [0x200c])


class Fnv(unittest.TestCase):
    def test_known(self):
        self.assertEqual(unityhooks.fnv1a(b''), 0xcbf29ce484222325)
        self.assertEqual(unityhooks.fnv1a(b'a'), 0xaf63dc4c8601ec8c)


if __name__ == '__main__':
    unittest.main()
