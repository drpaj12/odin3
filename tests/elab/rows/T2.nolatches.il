# generated: elab-check --write
attribute \top 1
module \t2_sensitivity
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y1
  wire output 5 \y2
  wire output 6 \y3
  wire output 7 \y4
  wire output 8 \y5
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
  wire $n19
  wire $n20
  wire $n21
  cell $and $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n10
  end
  cell $or $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n12
  end
  cell $xor $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n14
  end
  cell $xor $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n14
    connect \B \c
    connect \Y $n15
  end
  cell $and $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n17
  end
  cell $or $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n17
    connect \B \c
    connect \Y $n18
  end
  cell $and $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \b
    connect \B \c
    connect \Y $n20
  end
  cell $or $c20
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B $n20
    connect \Y $n21
  end
  process $proc$T2_event_lists.v:3$10
    assign $n9 $n10
    sync always
      update \y1 $n9
  end
  process $proc$T2_event_lists.v:5$12
    assign $n11 $n12
    sync always
      update \y2 $n11
  end
  process $proc$T2_event_lists.v:7$15
    assign $n13 $n15
    sync always
      update \y3 $n13
  end
  process $proc$T2_event_lists.v:9$18
    assign $n16 $n18
    sync always
      update \y4 $n16
  end
  process $proc$T2_event_lists.v:11$21
    assign $n19 $n21
    sync always
      update \y5 $n19
  end
end
