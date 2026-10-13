# generated: elab-check --write
attribute \top 1
module \c2_latch
  wire input 1 \en
  wire input 2 \d
  wire width 2 input 3 \s
  wire output 4 \q
  wire output 5 \y
  wire $n7
  wire $n8
  wire $n11
  wire $n12
  wire $n13
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \d
    connect \Y $n13
  end
  process $proc$C2_incomplete_if_case.v:3$8
    assign $n7 $n8
    switch \en
      case 1'1
        assign $n8 \d
      case
        assign $n8 1'x
    end
    sync always
      update \q $n7
  end
  process $proc$C2_incomplete_if_case.v:6$11
    assign $n11 $n12
    switch \s
      case 2'00
        assign $n12 \d
      case 2'01
        assign $n12 $n13
      case
        assign $n12 1'x
    end
    sync always
      update \y $n11
  end
end
