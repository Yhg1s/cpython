import contextlib
import unittest

class UnusedAssignmentTest(unittest.TestCase):
    def test_simple(self):
        ? = 1
        x, ?, z = range(3)
        self.assertEqual((x, z), (0, 2))
        x, *?, z = range(10)
        self.assertEqual((x, z), (0, 9))

    def test_for_loop(self):
        i = 0
        for ? in range(3):
            i += 1
        self.assertEqual(i, 3)
    
    def test_comprehension(self):
        l = [ 1 for ? in range(3) ]
        self.assertEqual(l, [1, 1, 1])
        l = list(1 for ? in range(3))
        self.assertEqual(l, [1, 1, 1])
        s = {1 for ? in range(3) }
        self.assertEqual(s, {1})
        d = {'k': 'v' for ? in range(3) }
        self.assertEqual(d, {'k': 'v'})

    def test_with(self):
        @contextlib.contextmanager
        def manager(n):
            yield range(n)
        with manager(10) as ?:
            pass
        with manager(3) as (x, ?, z):
            self.assertEqual((x, z), (0, 2))
        # TODO(twouters): figure out why this doesn't work.
        # with manager(10) as (x, *?, z):
        #     self.assertEqual((x, z), (0, 9))

if __name__ == '__main__':
    unittest.main()
