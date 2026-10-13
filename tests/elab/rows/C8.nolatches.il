# generated: elab-check --write
attribute \top 1
module \c8_non_constant_items
  wire width 4 input 1 \r
  wire width 2 input 2 \s
  wire width 2 input 3 \a
  wire width 2 input 4 \b
  wire width 2 output 5 \y
  wire output 6 \z
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n20
  wire $n21
  process $proc$C8_non_constant_items.v:3$9
    assign { $n15 $n14 } { $n17 $n16 }
    switch 1'1
      case \r [3]
        assign { $n17 $n16 } 2'11
      case \r [2]
        assign { $n17 $n16 } 2'10
      case \r [1]
        assign { $n17 $n16 } 2'01
      case
        assign { $n17 $n16 } 2'00
    end
    sync always
      update \y { $n15 $n14 }
  end
  process $proc$C8_non_constant_items.v:10$11
    assign $n20 $n21
    switch \s
      case \a
        assign $n21 1'1
      case \b
        assign $n21 1'0
      case
        assign $n21 1'x
    end
    sync always
      update \z $n20
  end
end
