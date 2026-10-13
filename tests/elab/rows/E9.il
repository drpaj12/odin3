# generated: elab-check --write
attribute \top 1
module \e9_case_equality
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire input 3 \c
  wire output 4 \y1
  wire output 5 \y2
  wire output 6 \y3
  wire output 7 \y4
  wire $n16
  cell $eq $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $ne $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \a [3:2]
    connect \B 2'10
    connect \Y \y3
  end
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n16
  end
  cell $eq $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n16
    connect \B \c
    connect \Y \y4
  end
end
