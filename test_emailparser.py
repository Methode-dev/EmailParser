import unittest
import os
import emailparser

MAIL_TXT = os.path.join(os.path.dirname(__file__), "mail.txt")


class TestEmailWithRealFile(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.segments = list(emailparser.Email(MAIL_TXT))

    # ── structure ────────────────────────────────────────────────────────────

    def test_segment_count(self):
        self.assertEqual(len(self.segments), 16)

    def test_all_segments_are_strings(self):
        for i, seg in enumerate(self.segments):
            self.assertIsInstance(seg, str, f"segment {i} is not a str")

    def test_no_empty_segments(self):
        for i, seg in enumerate(self.segments):
            self.assertGreater(len(seg), 0, f"segment {i} is empty")

    # ── first segment (latest reply) ─────────────────────────────────────────

    def test_first_segment_first_char_not_cut(self):
        self.assertTrue(self.segments[0].startswith("<"),
                        f"starts with {self.segments[0][:4]!r}")

    def test_first_segment_is_html(self):
        self.assertIn("mailMessageBodyContainer", self.segments[0])

    def test_first_segment_size(self):
        self.assertGreater(len(self.segments[0]), 5000)

    # ── separator detection ───────────────────────────────────────────────────

    def test_english_from_detected(self):
        # segments 1, 3, 4, 5, ... start with "From"
        self.assertTrue(self.segments[1].startswith("From:"))

    def test_french_de_detected(self):
        # segment 2 is a French-style "De :" separator
        self.assertTrue(self.segments[2].startswith("De"),
                        f"starts with {self.segments[2][:10]!r}")

    # ── known senders ─────────────────────────────────────────────────────────

    def test_sender_docs_interportfrance(self):
        self.assertIn("docs@interportfrance.fr", self.segments[1])

    def test_sender_info_interportfrance(self):
        self.assertIn("info@interportfrance.fr", self.segments[4])

    def test_sender_wilhelmsen(self):
        self.assertIn("wilhelmsen", self.segments[6].lower())

    def test_sender_priyanka_karthik(self):
        self.assertIn("priyanka.karthik@osmthome.com", self.segments[7])

    def test_sender_wps_osmthome_hub(self):
        self.assertIn("wps.osmthome.hub@wilhelmsen.com", self.segments[15])

    # ── iterator protocol ────────────────────────────────────────────────────

    def test_iter_returns_self(self):
        email = emailparser.Email(MAIL_TXT)
        self.assertIs(iter(email), email)

    def test_exhausted_raises_stop_iteration(self):
        email = emailparser.Email(MAIL_TXT)
        for _ in email:
            pass
        with self.assertRaises(StopIteration):
            next(email)

    # ── bytes input produces same result ─────────────────────────────────────

    def test_bytes_matches_file(self):
        with open(MAIL_TXT, "rb") as f:
            raw = f.read()
        self.assertEqual(list(emailparser.Email(raw)), self.segments)


class TestPlainText(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.segments = list(emailparser.Email(MAIL_TXT, plain_text=True))

    def test_segment_count(self):
        self.assertEqual(len(self.segments), 16)

    def test_no_html_tags(self):
        for i, seg in enumerate(self.segments):
            self.assertNotIn("<div", seg,  f"segment {i} still contains <div>")
            self.assertNotIn("<span", seg, f"segment {i} still contains <span>")

    def test_no_html_entities(self):
        for i, seg in enumerate(self.segments):
            self.assertNotIn("&lt;",  seg, f"segment {i} still contains &lt;")
            self.assertNotIn("&amp;", seg, f"segment {i} still contains &amp;")
            self.assertNotIn("&nbsp;", seg, f"segment {i} still contains &nbsp;")

    def test_first_segment_greeting(self):
        self.assertIn("Dear Ms. De Pedro", self.segments[0])

    def test_first_segment_sender_name(self):
        self.assertIn("ANDREA BALSERA", self.segments[0])

    def test_second_segment_sender_email(self):
        # HTML-encoded <docs@interportfrance.fr> should be decoded to plain
        self.assertIn("docs@interportfrance.fr", self.segments[1])

    def test_second_segment_no_angle_bracket_entities(self):
        self.assertNotIn("&lt;docs@", self.segments[1])

    def test_plain_text_same_count_as_html(self):
        html_count = len(list(emailparser.Email(MAIL_TXT)))
        self.assertEqual(len(self.segments), html_count)


class TestFindSignature(unittest.TestCase):

    def _sig(self, text):
        return emailparser.find_signature(text)

    # ── no signature ─────────────────────────────────────────────────────────

    def test_no_signature_returns_minus_one(self):
        self.assertEqual(self._sig("Just a normal message."), -1)

    def test_empty_string(self):
        self.assertEqual(self._sig(""), -1)

    # ── RFC delimiter ─────────────────────────────────────────────────────────

    def test_dash_dash_delimiter(self):
        text = "Hello.\n\n--\nJohn"
        idx = self._sig(text)
        self.assertEqual(text[idx:idx+2], "--")

    def test_dash_dash_space_delimiter(self):
        text = "Hello.\n\n-- \nJohn"
        idx = self._sig(text)
        self.assertEqual(text[idx:idx+3], "-- ")

    # ── English closings ──────────────────────────────────────────────────────

    def test_kind_regards(self):
        text = "Body text.\n\nKind regards,\nAlice"
        idx = self._sig(text)
        self.assertTrue(text[idx:].startswith("Kind regards"))

    def test_best_regards(self):
        text = "Body.\n\nBest regards,\nBob"
        idx = self._sig(text)
        self.assertTrue(text[idx:].startswith("Best regards"))

    def test_sincerely(self):
        text = "Body.\n\nSincerely,\nCarol"
        idx = self._sig(text)
        self.assertTrue(text[idx:].startswith("Sincerely"))

    def test_case_insensitive(self):
        text = "Body.\n\nKIND REGARDS,\nDave"
        self.assertNotEqual(self._sig(text), -1)

    def test_regards_not_matched_mid_word(self):
        self.assertEqual(self._sig("regardsomething is a word"), -1)

    # ── French closings ───────────────────────────────────────────────────────

    def test_cordialement(self):
        text = "Bonjour,\n\nCordialement,\nÉlodie"
        idx = self._sig(text)
        self.assertTrue(text[idx:].startswith("Cordialement"))

    def test_bien_cordialement(self):
        text = "Bonjour,\n\nBien cordialement,\nÉlodie"
        idx = self._sig(text)
        self.assertTrue(text[idx:].startswith("Bien cordialement"))

    # ── real file — plain text ────────────────────────────────────────────────

    def test_real_file_plain_text(self):
        seg = list(emailparser.Email(MAIL_TXT, plain_text=True))[0]
        idx = emailparser.find_signature(seg)
        self.assertNotEqual(idx, -1)
        self.assertTrue(seg[idx:].startswith("Kind regards"))

    def test_signature_splits_plain_body_correctly(self):
        seg = list(emailparser.Email(MAIL_TXT, plain_text=True))[0]
        idx = emailparser.find_signature(seg)
        self.assertIn("Yours below noted", seg[:idx])
        self.assertIn("ANDREA BALSERA",   seg[idx:])

    # ── real file — HTML (DOM path) ───────────────────────────────────────────

    def test_real_file_html_finds_signature(self):
        seg = list(emailparser.Email(MAIL_TXT))[0]  # plain_text=False
        idx = emailparser.find_signature(seg)
        self.assertNotEqual(idx, -1)

    def test_real_file_html_points_at_closing(self):
        seg = list(emailparser.Email(MAIL_TXT))[0]
        idx = emailparser.find_signature(seg)
        self.assertIn("Kind regards", seg[idx:idx + 30])

    def test_real_file_html_body_has_no_closing(self):
        seg = list(emailparser.Email(MAIL_TXT))[0]
        idx = emailparser.find_signature(seg)
        # the body (before idx) should not contain the closing phrase
        self.assertNotIn("Kind regards,</p>", seg[:idx])

    def test_html_inline_no_signature(self):
        self.assertEqual(self._sig("<p>Hello world</p>"), -1)

    def test_html_inline_with_closing(self):
        html = "<p>Body text.</p><p>Kind regards,</p><p>Alice</p>"
        idx  = self._sig(html)
        self.assertNotEqual(idx, -1)
        self.assertIn("Kind regards", html[idx:])

    def test_returns_int(self):
        self.assertIsInstance(self._sig("hello"), int)


if __name__ == "__main__":

    email = emailparser.Email(MAIL_TXT, plain_text=False)
    mail = next(email)
    s = emailparser.find_signature(mail)
    print(mail[s:])
    print('SEP\n\n\n\n\nSEP')
    mail = next(email)
    s = emailparser.find_signature(mail)
    print(mail[s:])
    exit()
    unittest.main(verbosity=2)