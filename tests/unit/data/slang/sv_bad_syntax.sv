// Fixture for the diagnostics review finding: a syntax error must reach the log with its text
// and its file:line, not be reported as a bare ODIN3_ERR_PARSE.
module bad_syntax (
    output logic y
);
    assign y = ;
endmodule
