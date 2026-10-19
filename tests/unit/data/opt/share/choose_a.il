module \top
  wire width 16 input 1 \b
  wire width 16 input 2 \a
  wire width 16 input 3 \c
  wire width 16 input 4 \d
  wire width 1 input 5 \s0
  wire width 1 input 6 \s1
  wire width 1 input 7 \s2
  wire width 16 output 8 \res
  wire width 16 \t0
  wire width 16 \t1
  wire width 16 \t2
  cell $and \and0
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \t0
  end
  cell $and \and1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \c
    connect \Y \t1
  end
  cell $and \and2
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \d
    connect \B \b
    connect \Y \t2
  end
  cell $pmux \p
    parameter \WIDTH 16
    parameter \S_WIDTH 3
    connect \A 16'xxxxxxxxxxxxxxxx
    connect \B { \t2 \t1 \t0 }
    connect \S { \s2 \s1 \s0 }
    connect \Y \res
  end
end
