module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y
  wire \t
  cell $reduce_or \r1
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \b \a }
    connect \Y \t
  end
  cell $reduce_or \r2
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \c \t }
    connect \Y \y
  end
end
