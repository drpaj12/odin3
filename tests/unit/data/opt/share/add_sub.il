module \top
  wire width 16 input 1 \a
  wire width 16 input 2 \b
  wire width 1 input 3 \sel
  wire width 16 output 4 \res
  wire width 16 \t1
  wire width 16 \t2
  cell $add \add1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \t1
  end
  cell $sub \sub1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \t2
  end
  cell $mux \m
    parameter \WIDTH 16
    connect \A \t2
    connect \B \t1
    connect \S \sel
    connect \Y \res
  end
end
