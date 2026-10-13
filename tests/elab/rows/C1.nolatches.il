# generated: elab-check --write
attribute \top 1
module \c1_if_else
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire width 2 input 4 \s
  wire output 5 \y1
  wire output 6 \y2
  wire output 7 \y3
  wire output 8 \y4
  wire $n10
  wire $n11
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n17
  end
  cell $eq $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n19
  end
  process $proc$C1_if_else.v:3$11
    assign $n10 $n11
    switch \a
      case 1'1
        assign $n11 \b
      case
        assign $n11 1'0
    end
    sync always
      update \y1 $n10
  end
  process $proc$C1_if_else.v:8$12
    assign $n14 $n15
    switch \a
      case 1'1
        assign $n15 \b
      case
        assign $n15 \c
    end
    sync always
      update \y2 $n14
  end
  process $proc$C1_if_else.v:13$15
    assign $n16 $n18
    switch $n17
      case 1'1
        assign $n18 \a
      case
        assign $n18 $n20
        switch $n19
          case 1'1
            assign $n20 \b
          case
            assign $n20 \c
        end
    end
    sync always
      update \y3 $n16
  end
  process $proc$C1_if_else.v:20$16
    assign $n21 $n22
    switch \a
      case 1'1
        assign $n22 $n23
        switch \b
          case 1'1
            assign $n23 \c
          case
            assign $n23 1'1
        end
      case
        assign $n22 1'0
    end
    sync always
      update \y4 $n21
  end
end
