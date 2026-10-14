attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  wire width 4 output 5 \z
  wire width 4 output 6 \u
  wire width 4 output 7 \v
  cell $mux $c10
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \u
  end
  cell $mux $c11
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \v
  end
  cell $add $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y
  end
  connect \z \y
end
