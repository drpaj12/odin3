attribute \top 1
module \t
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y
  wire \t
  cell $reduce_or $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \a \b }
    connect \Y \t
  end
  cell $reduce_or $c7
    parameter \A_SIGNED 0
    parameter \A_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A { \c \b \a }
    connect \Y \y
  end
end
