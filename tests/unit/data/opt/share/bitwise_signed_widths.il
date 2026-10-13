module \top
  wire width 4 input 1 \a
  wire width 2 input 2 \b
  wire width 3 input 3 \c
  wire input 4 \s
  wire width 4 output 5 \y
  wire width 4 \t1
  wire width 4 \t2
  cell $and \and1
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \t1
  end
  cell $and \and2
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \c
    connect \Y \t2
  end
  cell $mux \m
    parameter \WIDTH 4
    connect \A \t1
    connect \B \t2
    connect \S \s
    connect \Y \y
  end
end
