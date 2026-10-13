# generated: elab-check --write
attribute \top 1
module \c3_case_default
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 output 5 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  process $proc$C3_case_default.v:3$8
    assign { $n22 $n21 $n20 $n19 } { $n26 $n25 $n24 $n23 }
    switch \s
      case 2'00
        assign { $n26 $n25 $n24 $n23 } \a
      case 2'01
        assign { $n26 $n25 $n24 $n23 } \b
      case 2'10
        assign { $n26 $n25 $n24 $n23 } \c
      case
        assign { $n26 $n25 $n24 $n23 } 4'0000
    end
    sync always
      update \y { $n22 $n21 $n20 $n19 }
  end
end
