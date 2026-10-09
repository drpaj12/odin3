-- Uses one entity from each core.vhd; provenance must name the library, not only the file.
library ieee;
use ieee.std_logic_1164.all;
library alpha, beta;

entity top is
    port (a : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
    signal t : std_logic_vector(3 downto 0);
begin
    u_inv : entity alpha.inv4 port map (a => a, y => t);
    u_rot : entity beta.rot4 port map (a => t, y => y);
end architecture rtl;
