module \top
  wire width 16 input 1 \a
  wire width 16 input 2 \b
  wire width 16 input 3 \c
  wire width 16 input 4 \d
  wire width 1 input 5 \sel
  wire width 32 output 6 \res
  wire width 16 \p0
  wire width 16 \p1
  wire width 16 \q0
  wire width 16 \q1
  cell $add \add0
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \p0
  end
  cell $add \add1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \c
    connect \B \d
    connect \Y \p1
  end
  cell $sub \sub0
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \q0
  end
  cell $sub \sub1
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \c
    connect \B \d
    connect \Y \q1
  end
  cell $mux \m
    parameter \WIDTH 32
    connect \A { \q1 \q0 }
    connect \B { \p1 \p0 }
    connect \S \sel
    connect \Y \res
  end
end
