# generated: elab-check --write
attribute \top 1
module \c6_default_position
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  cell $xor $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n26 $n25 $n24 $n23 }
  end
  process $proc$C6_default_position_empty_item.v:3$9
    assign { $n18 $n17 $n16 $n15 } { $n22 $n21 $n20 $n19 }
    switch \s
      case 2'00
        assign { $n22 $n21 $n20 $n19 } \a
      case 2'01
        assign { $n22 $n21 $n20 $n19 } 4'xxxx
      case 2'10
        assign { $n22 $n21 $n20 $n19 } { $n26 $n25 $n24 $n23 }
      case
        assign { $n22 $n21 $n20 $n19 } \b
    end
    sync always
      update \y { $n18 $n17 $n16 $n15 }
  end
end
