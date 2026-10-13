module \m
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire input 4 \s
  wire width 4 output 5 \y
  wire width 4 \x1
  wire width 4 \x2
  cell $add \s1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \x1
  end
  cell $add \s2
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \c
    connect \Y \x2
  end
  cell $mux \mx
    parameter \WIDTH 4
    connect \A \x1
    connect \B \x2
    connect \S \s
    connect \Y \y
  end
end
