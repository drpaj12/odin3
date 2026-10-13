# generated: elab-check --write
attribute \top 1
module \c4_multi_item
  wire width 3 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
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
  wire $n27
  cell $and $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n27 $n26 $n25 $n24 }
  end
  process $proc$C4_multi_expression_items.v:3$8
    assign { $n19 $n18 $n17 $n16 } { $n23 $n22 $n21 $n20 }
    switch \s
      case 3'000 , 3'001 , 3'010
        assign { $n23 $n22 $n21 $n20 } \a
      case 3'011 , 3'100
        assign { $n23 $n22 $n21 $n20 } \b
      case 3'101
        assign { $n23 $n22 $n21 $n20 } { $n27 $n26 $n25 $n24 }
      case
        assign { $n23 $n22 $n21 $n20 } 4'0000
    end
    sync always
      update \y { $n19 $n18 $n17 $n16 }
  end
end
