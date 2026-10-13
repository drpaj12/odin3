# generated: elab-check --write
attribute \top 1
module \c9_xz_items
  wire width 2 input 1 \s
  wire width 2 output 2 \y
  wire $n5
  wire $n6
  wire $n7
  wire $n8
  process $proc$C9_xz_in_plain_case.v:3$7
    assign { $n6 $n5 } { $n8 $n7 }
    switch \s
      case 2'0x
        assign { $n8 $n7 } 2'01
      case 2'1z
        assign { $n8 $n7 } 2'10
      case
        assign { $n8 $n7 } 2'00
    end
    sync always
      update \y { $n6 $n5 }
  end
end
