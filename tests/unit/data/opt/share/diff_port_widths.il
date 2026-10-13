module \top
  wire width 16 input 1 \a
  wire width 16 input 2 \b
  wire width 16 input 3 \c
  wire width 1 input 4 \s0
  wire width 1 input 5 \s1
  wire width 1 input 6 \s2
  wire width 11 output 7 \res
  wire width 16 \t0
  wire width 16 \t1
  wire width 11 \t2
  cell $add \add0
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \t0
  end
  cell $add \add1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \c
    connect \Y \t1
  end
  cell $sub \sub2
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 11
    connect \A \a
    connect \B \b
    connect \Y \t2
  end
  cell $pmux \p
    parameter \WIDTH 11
    parameter \S_WIDTH 3
    connect \A 11'xxxxxxxxxxx
    connect \B { \t2 \t1 [10:0] \t0 [10:0] }
    connect \S { \s2 \s1 \s0 }
    connect \Y \res
  end
end
