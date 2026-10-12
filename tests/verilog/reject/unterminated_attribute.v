// An attribute without its closer. Pinned as Bison words it ("unexpected assign"): `(*` is a
// token, not a lexer body, and an error production would only follow Bison's own message.
module m (output y);
  (* keep = 1
  assign y = 0;
endmodule
