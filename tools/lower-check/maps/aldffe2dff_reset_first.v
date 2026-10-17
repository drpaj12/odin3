// yosys -- Yosys Open SYnthesis Suite
//
// Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
// Copyright (C) 2022  CAS—Atlantic (University of New Brunswick, CASA)
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Modified version of adff2dff for alddfe
//
// SPDX-License-Identifier: ISC
//
// 3B lower-check: the declared reference of PHASE3 decision 19 and 3B spec 6.3. This is Parmys's
// aldffe2dff.v (VTR 3c9a4d23, read-only under external/) with the next-state order changed: an active
// asynchronous reset or load wins over the enable (Peter's ruling 1 of PHASE3 #32), i.e.
//   Q <= ARST ? VALUE : (EN ? D : Q)
// where Parmys's own map has EN around the whole function (the $sdffce shape).

(* techmap_celltype = "$aldffe" *)
module aldffe2dff (CLK, ALOAD, AD, D, EN, Q);
	parameter WIDTH = 1;
	parameter CLK_POLARITY = 1;
	parameter ALOAD_POLARITY = 1;
	parameter EN_POLARITY = 1;

	input CLK, ALOAD, EN;
    (* force_downto *)
	input [WIDTH-1:0] AD;
	(* force_downto *)
	input [WIDTH-1:0] D;
	(* force_downto *)
	output reg [WIDTH-1:0] Q;
	(* force_downto *)
	reg [WIDTH-1:0] NEXT_Q;

	wire [1023:0] _TECHMAP_DO_ = "proc;;";

	always @*
		if (ALOAD == ALOAD_POLARITY)
			NEXT_Q <= AD;
		else if (EN == EN_POLARITY)
			NEXT_Q <= D;
		else
			NEXT_Q <= Q;

	if (CLK_POLARITY)
		always @(posedge CLK)
			Q <= NEXT_Q;
	else
		always @(negedge CLK)
			Q <= NEXT_Q;
endmodule
