# generated: elab-check --write
attribute \top 1
module \c5_casez_casex
  wire width 4 input 1 \r
  wire width 2 output 2 \y1
  wire width 2 output 3 \y2
  wire $n9
  wire $n10
  wire $n11
  wire $n12
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  process $proc$C5_casez_casex.v:3$6
    assign { $n10 $n9 } { $n12 $n11 }
    switch \r
      case 4'1---
        assign { $n12 $n11 } 2'11
      case 4'01--
        assign { $n12 $n11 } 2'10
      case 4'001-
        assign { $n12 $n11 } 2'01
      case
        assign { $n12 $n11 } 2'00
    end
    sync always
      update \y1 { $n10 $n9 }
  end
  process $proc$C5_casez_casex.v:10$7
    assign { $n16 $n15 } { $n18 $n17 }
    switch \r
      case 4'1---
        assign { $n18 $n17 } 2'11
      case 4'01--
        assign { $n18 $n17 } 2'10
      case 4'001-
        assign { $n18 $n17 } 2'01
      case
        assign { $n18 $n17 } 2'00
    end
    sync always
      update \y2 { $n16 $n15 }
  end
end
