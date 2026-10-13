module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire input 4 \s
  wire output 5 \y
  wire \t1
  wire \t2
  cell $and \and1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \t1
  end
  cell $and \and2
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \c
    connect \Y \t2
  end
  cell $mux \m
    parameter \WIDTH 1
    connect \A \t1
    connect \B \t2
    connect \S \s
    connect \Y \y
  end
end
