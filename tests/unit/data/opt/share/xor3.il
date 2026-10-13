module \top
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 4 input 4 \d
  wire width 1 input 5 \s0
  wire width 1 input 6 \s1
  wire width 1 input 7 \s2
  wire width 4 output 8 \y
  wire width 4 \t0
  wire width 4 \t1
  wire width 4 \t2
  cell $xor \x0
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \t0
  end
  cell $xor \x1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \c
    connect \Y \t1
  end
  cell $xor \x2
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \d
    connect \Y \t2
  end
  cell $pmux \p
    parameter \WIDTH 4
    parameter \S_WIDTH 3
    connect \A 4'0000
    connect \B { \t2 \t1 \t0 }
    connect \S { \s2 \s1 \s0 }
    connect \Y \y
  end
end
