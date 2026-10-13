# generated: elab-check --write
attribute \top 1
module \k5_func_extra
  wire width 4 input 1 \r
  wire width 2 output 2 \y
  wire $n7
  wire $n8
  wire $n9
  wire $n10
  wire $n11
  wire $n12
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  process $proc$K5_function_casez_localparam_block.v:16$5
    assign { $n12 $n11 $n10 $n9 } \r
    assign { $n14 $n13 } { $n18 $n17 }
    assign { $n16 $n15 } { $n18 $n17 }
    switch \r
      case 4'1---
        assign $n18 1'1
        assign $n17 1'1
      case 4'01--
        assign $n18 1'1
        assign $n17 1'0
      case 4'001-
        assign $n18 1'0
        assign $n17 1'1
      case
        assign $n18 1'0
        assign $n17 1'0
    end
    sync always
      update { $n8 $n7 } { $n16 $n15 }
  end
  connect \y { $n8 $n7 }
end
