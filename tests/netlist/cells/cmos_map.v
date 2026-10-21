// cmos_map.v: Yosys techmap from the gate-level internal cells to the cells of cmos.v (spec G-7).
// Used by gen.sh with Yosys 0.55: techmap -map cells/cmos_map.v

module \$_BUF_ (input A, output Y);
    BUF _TECHMAP_REPLACE_ (.A(A), .Y(Y));
endmodule

module \$_NOT_ (input A, output Y);
    NOT _TECHMAP_REPLACE_ (.A(A), .Y(Y));
endmodule

module \$_NAND_ (input A, input B, output Y);
    NAND2 _TECHMAP_REPLACE_ (.A(A), .B(B), .Y(Y));
endmodule

module \$_NOR_ (input A, input B, output Y);
    NOR2 _TECHMAP_REPLACE_ (.A(A), .B(B), .Y(Y));
endmodule

module \$_DFF_P_ (input C, input D, output Q);
    DFF _TECHMAP_REPLACE_ (.C(C), .D(D), .Q(Q));
endmodule

module \$_DFFSR_PPP_ (input C, input S, input R, input D, output Q);
    DFFSR _TECHMAP_REPLACE_ (.C(C), .D(D), .S(S), .R(R), .Q(Q));
endmodule
