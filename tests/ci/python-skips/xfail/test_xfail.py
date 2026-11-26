import unittest


class Xfail(unittest.TestCase):
    @unittest.expectedFailure
    def test_fails_as_expected(self):
        self.fail("planted expected failure")
