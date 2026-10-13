module \top
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 1 input 4 \s
  wire width 4 output 5 \y
  wire width 4 \t
  wire width 4 \u
  cell $add \addt
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \c
    connect \Y \t
  end
  cell $add \addu
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \u
  end
  cell $mux \m
    parameter \WIDTH 4
    connect \A { \t [0] \t [0] \t [0] \t [0] }
    connect \B \u
    connect \S \s
    connect \Y \y
  end
end
