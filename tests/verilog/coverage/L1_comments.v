// L1: comments // and /* */ — variants: whole-line comment, end-of-line comment, block comment, multi-line block comment, block comment between tokens, // inside a block comment, /* inside a line comment, comment after the last module
// a whole-line comment before the module
module l1_comments (input a, b, output y, z); // end-of-line comment
  /* block comment */
  /* multi-line block comment
     holding a // line-comment marker
     and more text */
  assign y = /* between tokens */ a;
  assign z = b; // a line comment holding /* an opener
endmodule
/* a block comment after the module */
