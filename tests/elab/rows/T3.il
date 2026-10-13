# generated: elab-check --write
attribute \top 1
module \t3_incomplete
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y
  wire output 5 \z
  wire $n6
  wire $n7
  wire $n8
  wire $n9
  wire $n10
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n7
  end
  cell $or $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n9
  end
  cell $or $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n9
    connect \B \c
    connect \Y $n10
  end
  process $proc$T3_incomplete_sensitivity.v:3$7
    assign $n6 $n7
    sync always
      update \y $n6
  end
  process $proc$T3_incomplete_sensitivity.v:5$10
    assign $n8 $n10
    sync always
      update \z $n8
  end
end
