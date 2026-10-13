module \top
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 1 input 4 \s
  wire width 1 input 5 \t0
  wire width 1 input 6 \t1
  wire width 4 output 7 \x
  wire width 4 output 8 \y
  wire width 4 \d
  wire width 4 \e
  cell $add \addd
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \d
  end
  cell $add \adde
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \c
    connect \Y \e
  end
  cell $mux \m1
    parameter \WIDTH 4
    connect \A \e
    connect \B \d
    connect \S \s
    connect \Y \x
  end
  cell $pmux \p
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \d
    connect \B { \b \a }
    connect \S { \t1 \t0 }
    connect \Y \y
  end
end
